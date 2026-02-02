// FLUTTER APP: BLE Chat Interface
// DEPENDENCY: flutter_blue_plus (Use modern syntax like FlutterBluePlus.startScan)
// TARGET UUID: "19B10000-E8F2-537E-4F6C-D104768A1214"

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'dart:convert';
import 'dart:io';
import 'package:permission_handler/permission_handler.dart';

void main() {
  runApp(const MyApp());
}

class MyApp extends StatelessWidget {
  const MyApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'BLE Chat',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: Colors.blue),
        useMaterial3: true,
      ),
      home: const ScanScreen(),
    );
  }
}

// ============ SCAN SCREEN ============
class ScanScreen extends StatefulWidget {
  const ScanScreen({super.key});

  @override
  State<ScanScreen> createState() => _ScanScreenState();
}

class _ScanScreenState extends State<ScanScreen> {
  final String targetServiceUUID = "19B10000-E8F2-537E-4F6C-D104768A1214";
  List<ScanResult> scanResults = [];
  bool isScanning = false;

  @override
  void initState() {
    super.initState();
    _requestPermissions();
  }

  Future<void> _requestPermissions() async {
    if (Platform.isAndroid) {
      final status = await [
        Permission.bluetoothScan,
        Permission.bluetoothConnect,
        Permission.location,
      ].request();
    }
  }

  Future<void> _startScan() async {
    if (!isScanning) {
      scanResults.clear();
      setState(() {
        isScanning = true;
      });

      try {
        await FlutterBluePlus.startScan(
          timeout: const Duration(seconds: 5),
          androidScanMode: AndroidScanMode.lowLatency,
        );

        // Listen to scan results
        FlutterBluePlus.scanResults.listen(
          (results) {
            setState(() {
              scanResults = results;
            });
          },
          onError: (error) {
            ScaffoldMessenger.of(context).showSnackBar(
              SnackBar(content: Text('Scan error: $error')),
            );
          },
        );
      } catch (e) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Failed to start scan: $e')),
        );
      } finally {
        // Scanning will stop automatically after timeout
        Future.delayed(const Duration(seconds: 5), () {
          if (mounted) {
            setState(() {
              isScanning = false;
            });
          }
        });
      }
    }
  }

  Future<void> _connectToDevice(BluetoothDevice device) async {
    try {
      await device.connect(timeout: const Duration(seconds: 10));
      if (mounted) {
        Navigator.of(context).push(
          MaterialPageRoute(
            builder: (context) => ChatScreen(device: device),
          ),
        );
      }
    } catch (e) {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Connection failed: $e')),
      );
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('BLE Scanner'),
        backgroundColor: Theme.of(context).colorScheme.inversePrimary,
      ),
      body: Center(
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            ElevatedButton(
              onPressed: isScanning ? null : _startScan,
              child: Text(isScanning ? 'Scanning...' : 'Start Scan'),
            ),
            const SizedBox(height: 20),
            Expanded(
              child: ListView.builder(
                itemCount: scanResults.length,
                itemBuilder: (context, index) {
                  final result = scanResults[index];
                  final deviceName =
                      result.device.platformName.isEmpty
                          ? 'Unknown'
                          : result.device.platformName;

                  // Check if device has target service
                  bool hasTargetService = result.advertisementData.serviceUuids
                      .contains(
                          Guid(targetServiceUUID));

                  return ListTile(
                    title: Text(deviceName),
                    subtitle: Text(
                      result.device.remoteId.toString(),
                      style: const TextStyle(fontSize: 12),
                    ),
                    trailing: Text('RSSI: ${result.rssi}'),
                    tileColor: hasTargetService
                        ? Colors.green.withOpacity(0.2)
                        : null,
                    onTap: () => _connectToDevice(result.device),
                  );
                },
              ),
            ),
          ],
        ),
      ),
    );
  }
}

// ============ CHAT SCREEN ============
class ChatScreen extends StatefulWidget {
  final BluetoothDevice device;

  const ChatScreen({super.key, required this.device});

  @override
  State<ChatScreen> createState() => _ChatScreenState();
}

class _ChatScreenState extends State<ChatScreen> {
  final TextEditingController _messageController = TextEditingController();
  List<String> messages = [];
  late BluetoothCharacteristic rxCharacteristic;
  late BluetoothCharacteristic txCharacteristic;
  bool isConnected = true;

  static const String SERVICE_UUID = "19B10000-E8F2-537E-4F6C-D104768A1214";
  static const String RX_CHAR_UUID = "19B10001-E8F2-537E-4F6C-D104768A1214";
  static const String TX_CHAR_UUID = "19B10002-E8F2-537E-4F6C-D104768A1214";

  @override
  void initState() {
    super.initState();
    _discoverServicesAndCharacteristics();
  }

  Future<void> _discoverServicesAndCharacteristics() async {
    try {
      List<BluetoothService> services =
          await widget.device.discoverServices();

      for (BluetoothService service in services) {
        if (service.uuid.toString().toUpperCase() ==
            SERVICE_UUID.toUpperCase()) {
          for (BluetoothCharacteristic characteristic
              in service.characteristics) {
            String charUuid = characteristic.uuid.toString().toUpperCase();

            if (charUuid == RX_CHAR_UUID.toUpperCase()) {
              rxCharacteristic = characteristic;
            } else if (charUuid == TX_CHAR_UUID.toUpperCase()) {
              txCharacteristic = characteristic;
              // Subscribe to TX characteristic for notifications
              await txCharacteristic.setNotifyValue(true);
              txCharacteristic.onValueReceived.listen(
                (value) {
                  String message = utf8.decode(value);
                  setState(() {
                    messages.add('Device: $message');
                  });
                },
              );
            }
          }
        }
      }

      // Listen for connection state changes
      widget.device.connectionState.listen((state) {
        if (state == BluetoothConnectionState.disconnected) {
          setState(() {
            isConnected = false;
          });
          if (mounted) {
            Navigator.of(context).pop();
            ScaffoldMessenger.of(context).showSnackBar(
              const SnackBar(content: Text('Device disconnected')),
            );
          }
        }
      });
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Service discovery failed: $e')),
        );
      }
    }
  }

  Future<void> _sendMessage() async {
    if (_messageController.text.isEmpty) return;

    try {
      String message = _messageController.text;
      List<int> bytes = utf8.encode(message);
      await rxCharacteristic.write(bytes, withoutResponse: false);

      setState(() {
        messages.add('You: $message');
      });

      _messageController.clear();
    } catch (e) {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Failed to send message: $e')),
      );
    }
  }

  @override
  void dispose() {
    _messageController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text(widget.device.platformName.isEmpty
            ? 'BLE Chat'
            : widget.device.platformName),
        backgroundColor: Theme.of(context).colorScheme.inversePrimary,
      ),
      body: Column(
        children: [
          // Message list
          Expanded(
            child: messages.isEmpty
                ? const Center(
                    child: Text('No messages yet'),
                  )
                : ListView.builder(
                    reverse: false,
                    itemCount: messages.length,
                    itemBuilder: (context, index) {
                      final message = messages[index];
                      final isUserMessage = message.startsWith('You:');

                      return Align(
                        alignment: isUserMessage
                            ? Alignment.centerRight
                            : Alignment.centerLeft,
                        child: Container(
                          margin: const EdgeInsets.all(8),
                          padding: const EdgeInsets.all(12),
                          decoration: BoxDecoration(
                            color: isUserMessage
                                ? Colors.blue
                                : Colors.grey[300],
                            borderRadius: BorderRadius.circular(12),
                          ),
                          child: Text(
                            message,
                            style: TextStyle(
                              color: isUserMessage
                                  ? Colors.white
                                  : Colors.black,
                            ),
                          ),
                        ),
                      );
                    },
                  ),
          ),
          // Input area
          Padding(
            padding: const EdgeInsets.all(8.0),
            child: Row(
              children: [
                Expanded(
                  child: TextField(
                    controller: _messageController,
                    decoration: InputDecoration(
                      hintText: 'Type a message...',
                      border: OutlineInputBorder(
                        borderRadius: BorderRadius.circular(8),
                      ),
                      contentPadding: const EdgeInsets.symmetric(
                        horizontal: 16,
                        vertical: 8,
                      ),
                    ),
                  ),
                ),
                const SizedBox(width: 8),
                ElevatedButton(
                  onPressed: isConnected ? _sendMessage : null,
                  child: const Text('Send'),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}



