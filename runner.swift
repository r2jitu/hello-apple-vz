// runner.swift — Host-side VM launcher for hello-apple-vz

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

let bootloader = VZLinuxBootLoader(kernelURL: kernelURL)
bootloader.commandLine = "console=hvc0"
config.bootLoader = bootloader

// Pipe: VZ writes guest console data here; we forward it to stdout.
let consolePipe = Pipe()
let serialPort = VZVirtioConsoleDeviceSerialPortConfiguration()
serialPort.attachment = VZFileHandleSerialPortAttachment(
    fileHandleForReading: FileHandle(forReadingAtPath: "/dev/null")!,
    fileHandleForWriting: consolePipe.fileHandleForWriting)
config.serialPorts = [serialPort]

// Forward pipe data to stdout as it arrives.
consolePipe.fileHandleForReading.readabilityHandler = { fh in
    let data = fh.availableData
    if !data.isEmpty { FileHandle.standardOutput.write(data) }
}

do { try config.validate() } catch { fatalError("VM config invalid: \(error)") }

var exitCode: Int32 = 0

class VMDelegate: NSObject, VZVirtualMachineDelegate {
    func guestDidStop(_ vm: VZVirtualMachine) {
        // Don't exit immediately — schedule exit 0.5 s later so the RunLoop
        // can deliver any pending VirtIO console data via readabilityHandler.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
            consolePipe.fileHandleForReading.readabilityHandler = nil
            consolePipe.fileHandleForWriting.closeFile()
            exit(exitCode)
        }
    }
    func virtualMachine(_ vm: VZVirtualMachine, didStopWithError e: Error) {
        fputs("VM error: \(e)\n", stderr)
        exitCode = 1
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { exit(1) }
    }
}

let delegate = VMDelegate()
let vm = VZVirtualMachine(configuration: config)
vm.delegate = delegate

print("Starting VM...")
vm.start { result in
    if case .failure(let e) = result { fatalError("Failed to start VM: \(e)") }
}

RunLoop.main.run()
