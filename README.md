# Printer Relay Logger

## Overview

This program acts as a relay between a printing application and a physical printer. Its primary purpose is to intercept and log the communication data sent to the printer for debugging or analysis purposes. It listens for incoming connections, forwards the data to the actual printer, and logs the exchanged data to files.

## Features

*   Relays print data from an application to a specified printer IP address.
*   Logs raw communication data to binary files in the `printer_data` directory.
*   Logs connection and status information to text files in the `printer_logs` directory.

## Setup

### Printer Configuration

To use this logger, you need to configure your printing application or operating system to send print jobs to this relay program instead of directly to the printer.

1.  **Identify the Printer's IP Address:** Find the actual IP address of your physical printer on your network (e.g., `192.168.1.100`). You will need this later.
2.  **Configure Printer Port:** In your operating system's printer settings, add a new TCP/IP port or modify the existing one for your target printer.
    *   Set the **IP Address** or **Hostname** to `127.0.0.1` (localhost).
    *   Ensure the **Port Number** is the standard raw printing port, typically `9100`.
    *   Make sure the **Protocol** is set to `Raw`.
3.  **Assign Port to Printer:** Assign this newly configured port (`127.0.0.1`, port `9100`) to the printer driver you want to monitor.

Now, when you print to this configured printer, the print job will be sent to `127.0.0.1:9100`, where the `Printer_Relay_Logger` should be running.

### Relay Program Configuration

The relay program (`Printer_Relay_Logger.exe`) needs to know where to listen for incoming connections and the actual IP address and port of your physical printer to forward the data.

Configuration is primarily handled via the `Printer_Relay_Logger.ini` file located in the same directory as the executable. If this file exists, the program will read the following settings from it:

*   `LocalHost`: The IP address the relay should listen on (default: `127.0.0.1`).
*   `LocalPort`: The port the relay should listen on (default: `9100`).
*   `RelayHost`: **(Required)** The IP address of the physical printer to relay data to.
*   `RelayPort`: The port on the physical printer to connect to (default: `9100`).

**Example `Printer_Relay_Logger.ini`:**

```ini
LocalHost = 127.0.0.1
LocalPort = 9100
RelayHost = 192.168.1.100 ; Replace with your printer's actual IP
RelayPort = 9100
```

**Fallback Configuration (Command Line):**

If the `Printer_Relay_Logger.ini` file is not found, or if it does not contain the `RelayHost` setting, the program requires the relay host IP address to be provided as a command-line argument:

```bash
Printer_Relay_Logger.exe <RELAY_HOST_IP>
```

Example: `Printer_Relay_Logger.exe 192.168.1.100`

In this fallback mode, the program uses the default `LocalHost` (`127.0.0.1`), `LocalPort` (`9100`), and `RelayPort` (`9100`).

## Usage

1.  Ensure the printer is configured to send jobs to the `LocalHost` and `LocalPort` specified in the configuration (default `127.0.0.1:9100`), as described in the "Printer Configuration" section.
2.  Configure the relay program by creating and editing `Printer_Relay_Logger.ini` with the correct `RelayHost` (your printer's IP). Alternatively, ensure the INI file is absent and you provide the printer's IP as a command-line argument.
3.  Run the executable:
    *   If using `Printer_Relay_Logger.ini`: `Printer_Relay_Logger.exe` (or use `run.bat` if available).
    *   If using command-line argument: `Printer_Relay_Logger.exe <RELAY_HOST_IP>` (e.g., `Printer_Relay_Logger.exe 192.168.1.100`).
4.  Send a print job from your application to the printer configured in the setup step.
5.  The relay program will intercept the job, log the data to the `printer_data` and `printer_logs` directories, and forward it to the physical printer specified by `RelayHost` and `RelayPort`.

## Compilation

To compile the program from the source code (`Printer_Relay_Logger.cpp`):

1.  Ensure you have the necessary C++ compiler and build tools installed (e.g., Visual Studio with C++ workload).
2.  Open a command prompt or terminal in the project directory.
3.  Run the build script:
    ```bash
    build.bat
    ```
4.  This will compile the source code and create the `Printer_Relay_Logger.exe` executable in the project directory.
