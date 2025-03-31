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

The relay program (`Printer_Relay_Logger.exe`) needs to know the actual IP address of your physical printer to forward the data. This is typically hardcoded or configured within the application itself (check the source code, e.g., `Printer_Relay_Logger.cpp`, for how the target printer IP is set).

## Usage

1.  Ensure the printer is configured to send jobs to `127.0.0.1` as described above.
2.  Make sure the `Printer_Relay_Logger.exe` is configured with the correct target printer IP address.
3.  Run the executable: `Printer_Relay_Logger.exe` or use `run.bat` if available.
4.  Send a print job from your application to the printer configured in the setup step.
5.  The relay program will intercept the job, log the data to the `printer_data` and `printer_logs` directories, and forward it to the physical printer.

## Compilation

To compile the program from the source code (`Printer_Relay_Logger.cpp`):

1.  Ensure you have the necessary C++ compiler and build tools installed (e.g., Visual Studio with C++ workload).
2.  Open a command prompt or terminal in the project directory.
3.  Run the build script:
    ```bash
    build.bat
    ```
4.  This will compile the source code and create the `Printer_Relay_Logger.exe` executable in the project directory.
