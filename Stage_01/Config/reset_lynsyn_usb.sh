#!/bin/bash
# Reset Lynsyn USB device
for device in $(lsusb | grep "10c4:8c1e" | cut -d' ' -f6 | tr ':' ' '); do
    echo "Resetting USB device $device"
    sudo /home/ansorosa/Desktop/CODE_REPOSITORIO/ERL_Stage_1_Framework_19_De-master-ModuloSoCLynsynTesting_GPU_LynsysWorking/Stage_01/Config/reset_lynsyn_usb.sh
    sleep 0.5
done