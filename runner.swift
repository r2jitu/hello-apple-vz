import Foundation
import Virtualization

#if !arch(arm64)
    fatalError("This requires an Apple Silicon Mac.")
#endif

let currentDir = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
let kernelURL = currentDir.appendingPathComponent("kernel.bin")

if !FileManager.default.fileExists(atPath: kernelURL.path) {
    fatalError("Missing kernel.bin. Run build.sh first.")
}

let config = VZVirtualMachineConfiguration()
config.cpuCount = 1
config.memorySize = 512 * 1024 * 1024 // 512 MB

let bootloader = VZLinuxBootLoader(kernelURL: kernelURL)
// Add dummy command line if needed, though not strictly necessary. Let's see if hypervisor fails without it.
bootloader.commandLine = "console=hvc0"
config.bootLoader = bootloader

let serialPort = VZVirtioConsoleDeviceSerialPortConfiguration()
// Provide /dev/null for reading so VZ properly initializes the console
let devNull = FileHandle(forReadingAtPath: "/dev/null")!
let attachment = VZFileHandleSerialPortAttachment(fileHandleForReading: devNull, fileHandleForWriting: FileHandle.standardOutput)
serialPort.attachment = attachment
config.serialPorts = [serialPort]

do {
    try config.validate()
} catch {
    fatalError("Invalid VM configuration: \(error)")
}

class VMDelegate: NSObject, VZVirtualMachineDelegate {
    func virtualMachine(_ virtualMachine: VZVirtualMachine, didStopWithError error: Error) {
        print("\n[Host] VM stopped with error: \(error)")
        exit(1)
    }

    func guestDidStop(_ virtualMachine: VZVirtualMachine) {
        print("\n[Host] VM Guest stopped cleanly.")
        exit(0)
    }
}

let delegate = VMDelegate()
let vm = VZVirtualMachine(configuration: config)
vm.delegate = delegate

print("Starting Virtual Machine...")
vm.start { result in
    switch result {
    case .success:
        print("[Host] VM started successfully.")
    case .failure(let error):
        print("[Host] Failed to start VM: \(error)")
        exit(1)
    }
}

RunLoop.main.run()
