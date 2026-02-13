using System.Text;
using System.Text.Json;
using MQTTnet;
using CommandService.Models;

namespace CommandService.Services;

public class MqttCommandWorker : BackgroundService
{
    private readonly ILogger<MqttCommandWorker> _logger;
    private readonly IConfiguration _config;
    private readonly CommandRepository _repository;

    private IMqttClient? _client;

    private readonly string _mqttHost;
    private readonly int _mqttPort;
    private readonly string _incomingTopic;
    private readonly string _outgoingTopic;

    // stanje uređaja po (house_id, device)
    // key = $"{houseId}:{device}", value = bool (true = ON, false = OFF)
    private readonly Dictionary<string, bool> _deviceStates = new();

    public MqttCommandWorker(
        ILogger<MqttCommandWorker> logger,
        IConfiguration config,
        CommandRepository repository)
    {
        _logger = logger;
        _config = config;
        _repository = repository;

        _mqttHost = _config["Mqtt:Host"] ?? "localhost";
        _mqttPort = int.TryParse(_config["Mqtt:Port"], out var p) ? p : 1883;
        _incomingTopic = _config["Mqtt:IncomingCommandsTopic"] ?? "home/house_1/commands";
        _outgoingTopic = _config["Mqtt:OutgoingCommandsTopic"] ?? "home/house_1/commands/actuators";
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        var factory = new MqttClientFactory();
        _client = factory.CreateMqttClient();

        var options = new MqttClientOptionsBuilder()
            .WithTcpServer(_mqttHost, _mqttPort)
            .WithClientId("CommandService")
            .Build();

        _client.ApplicationMessageReceivedAsync += async e =>
        {
            if (stoppingToken.IsCancellationRequested) return;

            try
            {
                var payload = e.ApplicationMessage.ConvertPayloadToString();

                _logger.LogInformation("[mqtt] Received on {Topic}: {Payload}", e.ApplicationMessage.Topic, payload);

                var list = JsonSerializer.Deserialize<List<CommandMessage>>(payload);
                var msg = list?.FirstOrDefault();

                if (msg is null)
                {
                    _logger.LogWarning("[mqtt] Deserialized CommandMessage is null, skipping.");
                    return;
                }

                await HandleCommandAsync(msg, stoppingToken);
            }
            catch (Exception ex)
            {
                _logger.LogError(ex, "[mqtt] Error while processing message");
            }
        };

        await _client.ConnectAsync(options, stoppingToken);
        _logger.LogInformation("[mqtt] Connected to {Host}:{Port}", _mqttHost, _mqttPort);

        await _client.SubscribeAsync(_incomingTopic, MQTTnet.Protocol.MqttQualityOfServiceLevel.AtMostOnce, stoppingToken);
        _logger.LogInformation("[mqtt] Subscribed to {Topic}", _incomingTopic);

        while (!stoppingToken.IsCancellationRequested)
        {
            await Task.Delay(1000, stoppingToken);
        }
    }

    private async Task HandleCommandAsync(CommandMessage msg, CancellationToken ct)
    {
        if (string.IsNullOrWhiteSpace(msg.Device) || string.IsNullOrWhiteSpace(msg.Command))
        {
            _logger.LogWarning("[cmd] Missing device/command in message, skipping.");
            return;
        }

        var houseId = msg.HouseId ?? "unknown_house";
        var key = $"{houseId}:{msg.Device}";

        bool desiredOn;
        if (msg.Command.StartsWith("TURN_ON", StringComparison.OrdinalIgnoreCase))
        {
            desiredOn = true;
        }
        else if (msg.Command.StartsWith("TURN_OFF", StringComparison.OrdinalIgnoreCase))
        {
            desiredOn = false;
        }
        else
        {
            _logger.LogWarning("[cmd] Unknown command value: {Command}", msg.Command);
            return;
        }

        _deviceStates.TryGetValue(key, out var currentState);
        var isKnown = _deviceStates.ContainsKey(key);

        if (isKnown && currentState == desiredOn)
        {
            _logger.LogInformation("[cmd] Ignored command for {Key}, state unchanged ({State}).", key, desiredOn ? "ON" : "OFF");
            return;
        }

        _deviceStates[key] = desiredOn;
        _logger.LogInformation("[cmd] State updated for {Key} => {State}", key, desiredOn ? "ON" : "OFF");

        await _repository.InsertCommandAsync(msg, desiredOn, ct);

        await PublishToArduinoAsync(msg, desiredOn, ct);
    }

    private async Task PublishToArduinoAsync(CommandMessage msg, bool desiredOn, CancellationToken ct)
    {
        if (_client is null || !_client.IsConnected)
        {
            _logger.LogWarning("[mqtt] Client not connected, cannot publish to Arduino.");
            return;
        }

        var payloadObj = new
        {
            device = msg.Device,
            command = msg.Command,
            desired_state = desiredOn ? "ON" : "OFF",
            reason = msg.Reason,
            house_id = msg.HouseId,
            user_id = msg.UserId,
            ts_server = DateTime.UtcNow
        };

        var json = JsonSerializer.Serialize(payloadObj);
        var message = new MqttApplicationMessageBuilder()
            .WithTopic(_outgoingTopic)
            .WithPayload(json)
            .WithQualityOfServiceLevel(MQTTnet.Protocol.MqttQualityOfServiceLevel.AtMostOnce)
            .Build();

        await _client.PublishAsync(message, ct);
        _logger.LogInformation("[mqtt] Published to Arduino topic {Topic}: {Payload}", _outgoingTopic, json);
    }

    public override async Task StopAsync(CancellationToken cancellationToken)
    {
        if (_client != null)
        {
            try
            {
                if (_client.IsConnected)
                {
                    await _client.DisconnectAsync(cancellationToken: CancellationToken.None);
                    _logger.LogInformation("[mqtt] Disconnected from broker.");
                }
            }
            catch (Exception ex)
            {
                _logger.LogError(ex, "[mqtt] Error during disconnect");
            }
        }

        await base.StopAsync(cancellationToken);
    }
}
