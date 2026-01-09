// FLUTTER APP: BLE Chat Interface with Call & Notification Forwarding
// DEPENDENCY: flutter_blue_plus (Use modern syntax like FlutterBluePlus.startScan)
// TARGET UUID: "19B10000-E8F2-537E-4F6C-D104768A1214"

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'dart:convert';
import 'dart:io';
import 'package:permission_handler/permission_handler.dart';
import 'package:installed_apps/installed_apps.dart';
import 'package:installed_apps/app_info.dart';

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

// ============ NOTIFICATION FILTER MANAGER ============
class NotificationFilterManager {
  static final NotificationFilterManager _instance = NotificationFilterManager._internal();
  
  factory NotificationFilterManager() {
    return _instance;
  }
  
  NotificationFilterManager._internal();
  
  final Set<String> enabledApps = {};
  
  void toggleApp(String appName) {
    if (enabledApps.contains(appName)) {
      enabledApps.remove(appName);
    } else {
      enabledApps.add(appName);
    }
  }
  
  bool isAppEnabled(String appName) {
    return enabledApps.contains(appName);
  }
}

// ============ SCAN SCREEN ============
class ScanScreen extends StatefulWidget {
  const ScanScreen({super.key});

  @override
  State<ScanScreen> createState() => _ScanScreenState();
}

class _ScanScreenState extends State<ScanScreen> {
  // Match firmware service UUID (see Hardware_software/src/main.cpp)
  final String targetServiceUUID = "819B2F01-9D7D-42F1-A58E-29E5C07DD6B6";
  List<ScanResult> scanResults = [];
  bool isScanning = false;
  
  static const platform = MethodChannel('com.example.app/events');

  @override
  void initState() {
    super.initState();
    _requestPermissionsAndSetup();
  }

  Future<void> _requestPermissionsAndSetup() async {
    // Request runtime permissions
    await _requestRuntimePermissions();
    
    // Check and request Notification Listener access
    await _checkNotificationListenerAccess();
  }
  
  Future<void> _requestRuntimePermissions() async {
    if (Platform.isAndroid) {
      final statuses = await [
        Permission.bluetoothScan,
        Permission.bluetoothConnect,
        Permission.location,
        Permission.phone,  // READ_PHONE_STATE
      ].request();
      
      // Log permission results
      statuses.forEach((permission, status) {
        print('$permission: $status');
      });
    }
  }
  
  Future<void> _checkNotificationListenerAccess() async {
    if (!Platform.isAndroid) return;
    
    try {
      final bool isEnabled = await platform.invokeMethod('isNotificationListenerEnabled') ?? false;
      
      if (!isEnabled && mounted) {
        _showNotificationListenerDialog();
      }
    } catch (e) {
      print('Error checking notification listener: $e');
      // Still show the dialog to be safe
      if (mounted) {
        _showNotificationListenerDialog();
      }
    }
  }
  
  void _showNotificationListenerDialog() {
    showDialog(
      context: context,
      barrierDismissible: false,
      builder: (context) => AlertDialog(
        title: const Text('Enable Notification Listener'),
        content: const Text(
          'This app needs access to notifications to forward them to your nRF52 device.\n\n'
          'You will be taken to Settings where you need to:\n'
          '1. Find "Notification access"\n'
          '2. Enable access for this app\n\n'
          'This is required for the notification feature to work.',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context),
            child: const Text('Later'),
          ),
          ElevatedButton(
            onPressed: () {
              _openNotificationSettings();
              Navigator.pop(context);
            },
            child: const Text('Open Settings'),
          ),
        ],
      ),
    );
  }
  
  Future<void> _openNotificationSettings() async {
    try {
      // Try to open notification access settings
      await platform.invokeMethod('openNotificationSettings');
    } catch (e) {
      print('Error opening notification settings: $e');
      // Fallback: show instructions
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(
            content: Text('Open Settings > Apps & notifications > Special app access > Notification access'),
            duration: Duration(seconds: 5),
          ),
        );
      }
    }
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
  BluetoothCharacteristic? rxCharacteristic; // write target
  BluetoothCharacteristic? txCharacteristic; // notify source
  bool isConnected = true;
  bool isReady = false;
  
  // Method channel for Android native events
  static const platform = MethodChannel('com.example.app/events');
  
  // Track all seen apps
  final Set<String> allAppsSet = {};
  // Map packageName -> human-readable label
  final Map<String, String> _appLabels = {};

  // Match firmware UUIDs (see Hardware_software/src/main.cpp)
  static const String SERVICE_UUID = "819B2F01-9D7D-42F1-A58E-29E5C07DD6B6";
  static const String RX_CHAR_UUID = "5600F473-A667-4C60-B0B1-7C68FBE9720F"; // RX write
  static const String TX_CHAR_UUID = "B2FD3BE9-40CA-48CD-80ED-5D04B2A71DA2"; // TX notify

  @override
  void initState() {
    super.initState();
    _discoverServicesAndCharacteristics();
    _setupEventListeners();
    _loadInstalledApps();
  }

  Future<void> _loadInstalledApps() async {
    if (!Platform.isAndroid) return;
    try {
      final List<AppInfo> apps = await InstalledApps.getInstalledApps(
        excludeSystemApps: true,
        excludeNonLaunchableApps: true,
        withIcon: false,
      );
      if (!mounted) return;
      setState(() {
        for (final app in apps) {
          _appLabels[app.packageName] = app.name;
          allAppsSet.add(app.packageName);
        }
      });
    } catch (e) {
      debugPrint('Failed to load installed apps: $e');
    }
  }
  
  void _setupEventListeners() {
    platform.setMethodCallHandler((call) async {
      if (call.method == 'onCall') {
        final String phoneNumber = call.arguments['phoneNumber'];
        final int duration = call.arguments['duration'];
        _handleIncomingCall(phoneNumber, duration);
      } else if (call.method == 'onNotification') {
        final String appName = call.arguments['appName'];
        final String title = call.arguments['title'];
        final String message = call.arguments['message'];
        _handleNotification(appName, title, message);
      }
    });
  }
  
  void _handleIncomingCall(String phoneNumber, int duration) {
    if (!mounted) return;
    final String callData = 'CALL:$phoneNumber:${duration}s';
    print('Incoming call: $callData');
    
    setState(() {
      messages.add('📞 Call from $phoneNumber (${duration}s)');
    });
    
    _sendToDevice(callData);
  }
  
  void _handleNotification(String appName, String title, String message) {
    if (!mounted) return;
    // appName is expected to be the package name from Android native
    setState(() {
      allAppsSet.add(appName);
    });
    
    // Check if app is enabled in filter
    if (!NotificationFilterManager().isAppEnabled(appName)) {
      print('Notification from $appName filtered out');
      return;
    }
    
    final String notifData = 'NOTIF:$appName:$title:$message';
    print('Notification forwarded: $notifData');
    
    setState(() {
      messages.add('🔔 [$appName] $title');
    });
    
    _sendToDevice(notifData);
  }
  
  Future<void> _sendToDevice(String data) async {
    if (rxCharacteristic == null) return;
    
    try {
      List<int> bytes = utf8.encode(data);
      const int chunkSize = 20;
      
      for (int i = 0; i < bytes.length; i += chunkSize) {
        int end = (i + chunkSize < bytes.length) ? i + chunkSize : bytes.length;
        List<int> chunk = bytes.sublist(i, end);
        await rxCharacteristic!.write(chunk, withoutResponse: false);
        await Future.delayed(const Duration(milliseconds: 50));
      }
    } catch (e) {
      print('Error sending to device: $e');
    }
  }

  Future<void> _discoverServicesAndCharacteristics() async {
    try {
      List<BluetoothService> services =
          await widget.device.discoverServices();

      for (BluetoothService service in services) {
        if (service.uuid.toString().toUpperCase() ==
            SERVICE_UUID.toUpperCase()) {
          print('Found target service: $SERVICE_UUID');
          for (BluetoothCharacteristic characteristic
              in service.characteristics) {
            String charUuid = characteristic.uuid.toString().toUpperCase();
            print('Found characteristic: $charUuid (write: ${characteristic.properties.write}, writeWithoutResponse: ${characteristic.properties.writeWithoutResponse}, notify: ${characteristic.properties.notify})');

            // Explicit UUID matches first
            if (charUuid == RX_CHAR_UUID.toUpperCase()) {
              rxCharacteristic = characteristic;
            } else if (charUuid == TX_CHAR_UUID.toUpperCase()) {
              txCharacteristic = characteristic;
            } else {
              // Fallback by properties: if it can write, treat as RX; if it can notify/indicate, treat as TX
              final canWrite =
                  characteristic.properties.write ||
                      characteristic.properties.writeWithoutResponse;
              final canNotify =
                  characteristic.properties.notify ||
                      characteristic.properties.indicate;
              rxCharacteristic ??= canWrite ? characteristic : null;
              txCharacteristic ??= canNotify ? characteristic : null;
            }
          }
          print('RX Characteristic found: ${rxCharacteristic != null}');
          print('TX Characteristic found: ${txCharacteristic != null}');
          final notifyChar = txCharacteristic ?? rxCharacteristic;
          if (notifyChar != null &&
              (notifyChar.properties.notify ||
                  notifyChar.properties.indicate)) {
            await notifyChar.setNotifyValue(true);
            notifyChar.onValueReceived.listen(
              (value) {
                String message = utf8.decode(value);
                setState(() {
                  messages.add('Device: $message');
                });
              },
            );
          }

          if (rxCharacteristic != null) {
            setState(() {
              isReady = true;
            });
          }
        }
      }

      if (!isReady) {
        if (mounted) {
          ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(content: Text('Required BLE characteristic not found')),
          );
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
    if (rxCharacteristic == null) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Characteristic not ready yet')),
      );
      return;
    }
    try {
      String message = _messageController.text;
      List<int> bytes = utf8.encode(message);
      print('Attempting to write to RX: $message (${bytes.length} bytes)');
      print('RX Characteristic is null: ${rxCharacteristic == null}');
      
      // Split into 20-byte chunks to avoid MTU limit
      const int chunkSize = 20;
      for (int i = 0; i < bytes.length; i += chunkSize) {
        int end = (i + chunkSize < bytes.length) ? i + chunkSize : bytes.length;
        List<int> chunk = bytes.sublist(i, end);
        print('Writing chunk ${i ~/ chunkSize + 1}: ${chunk.length} bytes');
        // Use write with response 
        await rxCharacteristic!.write(chunk, withoutResponse: false);
        // Small delay between chunks to avoid overwhelming the device
        await Future.delayed(const Duration(milliseconds: 50));
      }
      print('Write succeeded');

      setState(() {
        messages.add('You: $message');
      });

      _messageController.clear();
    } catch (e) {
      print('Write error: $e');
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
  
  void _showNotificationFilterDialog() {
    final filterManager = NotificationFilterManager();
    // Build (package -> label) entries and sort by label
    final List<MapEntry<String, String>> entries = allAppsSet
        .map((pkg) => MapEntry(pkg, _appLabels[pkg] ?? pkg))
        .toList()
      ..sort((a, b) => a.value.toLowerCase().compareTo(b.value.toLowerCase()));
    final searchController = TextEditingController();
    final List<MapEntry<String, String>> filteredEntries = List.from(entries);
    
    showDialog(
      context: context,
      builder: (context) => StatefulBuilder(
        builder: (context, setState) {
          return AlertDialog(
            title: const Text('Filter Notifications'),
            content: SingleChildScrollView(
              child: SizedBox(
                width: double.maxFinite,
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    TextField(
                      controller: searchController,
                      decoration: InputDecoration(
                        hintText: 'Search apps...',
                        prefixIcon: const Icon(Icons.search),
                        border: OutlineInputBorder(
                          borderRadius: BorderRadius.circular(8),
                        ),
                      ),
                      onChanged: (query) {
                        setState(() {
                          if (query.isEmpty) {
                            filteredEntries
                              ..clear()
                              ..addAll(entries);
                          } else {
                            final q = query.toLowerCase();
                            filteredEntries
                              ..clear()
                              ..addAll(entries.where((e) =>
                                  e.value.toLowerCase().contains(q) ||
                                  e.key.toLowerCase().contains(q)));
                          }
                        });
                      },
                    ),
                    const SizedBox(height: 16),
                    SizedBox(
                      height: 300,
                      child: filteredEntries.isEmpty
                          ? const Center(
                              child: Text('No apps found'),
                            )
                          : ListView.builder(
                              itemCount: filteredEntries.length,
                              itemBuilder: (context, index) {
                                final entry = filteredEntries[index];
                                final packageName = entry.key;
                                final label = entry.value;
                                final isEnabled = filterManager.isAppEnabled(packageName);
                                
                                return CheckboxListTile(
                                  title: Text(label),
                                  subtitle: Text(packageName, style: const TextStyle(fontSize: 12)),
                                  value: isEnabled,
                                  onChanged: (bool? value) {
                                    setState(() {
                                      filterManager.toggleApp(packageName);
                                    });
                                  },
                                );
                              },
                            ),
                    ),
                  ],
                ),
              ),
            ),
            actions: [
              TextButton(
                onPressed: () => Navigator.pop(context),
                child: const Text('Close'),
              ),
            ],
          );
        },
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text(widget.device.platformName.isEmpty
            ? 'BLE Chat'
            : widget.device.platformName),
        backgroundColor: Theme.of(context).colorScheme.inversePrimary,
        actions: [
          IconButton(
            icon: const Icon(Icons.notifications),
            onPressed: _showNotificationFilterDialog,
            tooltip: 'Filter Notifications',
          ),
        ],
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
                  onPressed: (isConnected && isReady) ? _sendMessage : null,
                  child: Text(isReady ? 'Send' : 'Connecting...'),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}



