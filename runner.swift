// runner.swift — Host-side VM launcher for hello-apple-vz
//
// Configures and starts an Apple Virtualization.framework VM that boots
// kernel.bin via VZLinuxBootLoader. Guest VirtIO console output is forwarded
// to the host's standard output.
//
// Exit sequence:
//   psci_off (guest) → guestDidStop (VZ callback) → closes our pipe write end
//   VZ drains its I/O thread → closes its pipe write end (EOF)
//   readabilityHandler sees EOF → exit(0)
// This ordering ensures all console output is forwarded before the process exits.

import Foundation
import Virtualization

#if !arch(arm64)
fatalError("Apple Virtualization.framework requires Apple Silicon.")
#endif

let kernelURL = URL(fileURLWithPath: "kernel.bin",
                    relativeTo: URL(fileURLWithPath: FileManager.default.currentDirectoryPath))
guard FileManager.default.fileExists(atPath: kernelURL.path) else {
    fatalError("kernel.bin not found — run `bash build.sh` first.")
}

let config = VZVirtualMachineConfiguration()
config.cpuCount   = 1
config.memorySize = 256 * 1024 * 1024

// Boot using the Linux/ARM64 boot protocol; FDT is passed in x0.
let bootloader = VZLinuxBootLoader(kernelURL: kernelURL)
config.bootLoader = bootloader

// Route guest console through a Pipe.
let consolePipe = Pipe()
let serialPort = VZVirtioConsoleDeviceSerialPortConfiguration()
serialPort.attachment = VZFileHandleSerialPortAttachment(
    fileHandleForReading: FileHandle(forReadingAtPath: "/dev/null")!,
    fileHandleForWriting: consolePipe.fileHandleForWriting)
config.serialPorts = [serialPort]

do { try config.validate() } catch { fatalError("VM config invalid: \(error)") }

class VMDelegate: NSObject, VZVirtualMachineDelegate {
    func guestDidStop(_ vm: VZVirtualMachine) {
        // Close our copy of the write end. VZ holds its own copy and will
        // close it when the VM finishes tearing down. Once both ends close
        // the pipe reaches EOF, and readabilityHandler below calls exit(0).
        consolePipe.fileHandleForWriting.closeFile()
    }
    func virtualMachine(_ vm: VZVirtualMachine, didStopWithError e: Error) {
        fputs("VM error: \(e)\n", stderr); exit(1)
    }
}

let delegate = VMDelegate()
let vm = VZVirtualMachine(configuration: config)
vm.delegate = delegate

// Forward console data to stdout. When the pipe reaches EOF (all write ends
// closed after the VM stops), exit cleanly.
consolePipe.fileHandleForReading.readabilityHandler = { fh in
    let data = fh.availableData
    if data.isEmpty { exit(0) }   // EOF — VM stopped, all output forwarded
    FileHandle.standardOutput.write(data)
}

vm.start { result in
    if case .failure(let e) = result { fatalError("Failed to start VM: \(e)") }
}

RunLoop.main.run()
