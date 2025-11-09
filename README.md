# multi_track - Max/MSP External

A Max/MSP external for multi-track audio source separation using neural networks via a Python server.

## Overview

`multi_track` enables real-time neural network inference for separating audio into four instrument stems:
- Bass
- Drums
- Guitar
- Piano

The external communicates with a Python server via OSC (Open Sound Control) to leverage GPU-accelerated processing.

## Requirements

- Max/MSP 8 or later
- Python 3.x with required libraries (see Python server documentation)
- Visual Studio 2022 (for building)
- oscpack library

## Building

### Windows

1. Ensure oscpack is built in `max-sdk-main/oscpack_1_1_0/build/Debug/`
2. Open the project in Visual Studio or use CMake:
   ```
   cmake --build build --config Debug
   ```
3. The compiled external will be in `max-sdk-main/externals/multi_track.mxe64`

## Usage

### Basic Setup

1. **Set the server command:**
   ```
   set_command <python_command>
   ```
   Example: `set_command "python server.py --server_ip 192.168.1.100"`

2. **Start the Python server:**
   ```
   server 1
   ```
   The external will automatically load the model when the server is ready.

3. **Stop the server:**
   ```
   server 0
   ```

### Configuration Messages

- `verbose <0|1>` - Enable/disable verbose logging and timing information
- `percentage <0.0-1.0>` - Set processing percentage
- `pr_win_mul <0.0-2.0>` - Set processing window multiplier
- `packet_size <128-16384>` - Set OSC packet size in floats
- `predict_instruments <bass> <drums> <guitar> <piano>` - Select which instruments to predict (1=yes, 0=no)
- `read_instruments <bass> <drums> <guitar> <piano>` - Select which instruments to read from input (1=yes, 0=no)

### Processing Audio

Send a 4-plane Jitter matrix to the external:
```
jit_matrix <matrix_name>
```

The matrix must be:
- Type: `float32`
- Planes: 4 (one for each instrument)

### Testing

- `test_packet` - Test OSC packet transmission and measure round-trip time
- `print` - Send print command to Python server
- `reset` - Reset all instrument indices
- `get_client_ip` - Get and display the client's public IP address

### Manual Model Loading

If needed, you can manually load the model:
```
load_model
```

## Outlets (Right to Left)

1. **Bass** (leftmost) - Outputs bass audio data as index/value pairs
2. **Drums** - Outputs drums audio data
3. **Guitar** - Outputs guitar audio data
4. **Piano** (rightmost) - Outputs piano audio data

## Timing & Performance

When `verbose 1` is enabled, the external displays:
- Data import time (matrix send time)
- Total round-trip processing time
- Packet test round-trip times
- Network throughput statistics

## Network Configuration

The external automatically detects:
- **Client IP**: Your public IP (via api.ipify.org)
- **Server IP**: Set via `set_command` with `--server_ip` flag

Default ports:
- Sender: 7000
- Listener: 8000

## Python Server Requirements

Your Python server must:
1. Send OSC message `/ready <true>` when initialization is complete
2. Accept OSC messages on port 7000 (or configured sender port)
3. Send responses to port 8000 (or configured listener port)
4. Send `/server_predicted <true>` when processing is complete

### Expected OSC Messages from Server

- `/ready <bool>` - Server is ready to accept commands
- `/server_predicted <bool>` - Processing complete
- `/packet_test_response <size> <float[]>` - Response to packet test
- `/bass <index> <float[]>` - Bass audio data
- `/drums <index> <float[]>` - Drums audio data
- `/guitar <index> <float[]>` - Guitar audio data
- `/piano <index> <float[]>` - Piano audio data

## Troubleshooting

- **Build errors**: Ensure oscpack is built for both Debug and Release configurations
- **Server won't start**: Check that `set_command` was called with a valid Python command
- **No audio output**: Enable `verbose 1` to see timing and data flow
- **Packet errors**: Try adjusting `packet_size` (default: 10240)

## Author

Torniko Karchkhadze (tkarchkhadze@ucsd.edu)

## License

See Max SDK license
