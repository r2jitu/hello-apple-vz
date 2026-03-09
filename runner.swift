// runner.swift — Host-side VM launcher for hello-apple-vz
//
// Configures and starts an Apple Virtualization.framework VM that boots
// kernel.bin via VZLinuxBootLoader. Guest VirtIO console output is forwarded
// to the host's standard output.

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
bootloader.commandLine = "console=hvc0"
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
    var vm: VZVirtualMachine?

    func guestDidStop(_ vm: VZVirtualMachine) {
        consolePipe.fileHandleForReading.readabilityHandler = nil
        consolePipe.fileHandleForWriting.closeFile()
        let tail = consolePipe.fileHandleForReading.readDataToEndOfFile()
        if !tail.isEmpty { FileHandle.standardOutput.write(tail) }
        exit(0)
    }
    func virtualMachine(_ vm: VZVirtualMachine, didStopWithError e: Error) {
        fputs("VM error: \(e)\n", stderr); exit(1)
    }
}

let delegate = VMDelegate()
let vm = VZVirtualMachine(configuration: config)
vm.delegate = delegate
delegate.vm = vm

// Forward console data to stdout.
consolePipe.fileHandleForReading.readabilityHandler = { fh in
    let data = fh.availableData
    guard !data.isEmpty else { return }
    FileHandle.standardOutput.write(data)
    DispatchQueue.main.async { vm.stop { _ in exit(0) } }
}

vm.start { result in
    if case .failure(let e) = result { fatalError("Failed to start VM: \(e)") }
}

RunLoop.main.run()
