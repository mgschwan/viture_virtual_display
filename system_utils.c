#include "system_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

const char* udev_rules_path = "/etc/udev/rules.d/99-xr_virtual_display.rules";
const char* udev_rules_content = "SUBSYSTEMS==\"usb\", ENV{DEVTYPE}==\"usb_device\", ATTRS{idVendor}==\"35ca\", ATTRS{idProduct}==\"101d\", MODE=\"0666\", GROUP=\"plugdev\"\n";

void install_udev_rules() {
    struct stat st;
    if (stat(udev_rules_path, &st) == 0) {
        printf("udev rules file already exists: %s\n", udev_rules_path);
        return;
    }

    FILE *file = fopen(udev_rules_path, "w");
    if (file == NULL) {
        if (errno == EACCES) {
            fprintf(stderr, "Error: Permission denied. Please run with sudo to install udev rules.\n");
        } else {
            fprintf(stderr, "Error: Failed to open udev rules file for writing: %s\n", strerror(errno));
        }
        exit(EXIT_FAILURE);
    }

    if (fputs(udev_rules_content, file) == EOF) {
        fprintf(stderr, "Error: Failed to write to udev rules file.\n");
        fclose(file);
        exit(EXIT_FAILURE);
    }

    fclose(file);
    printf("udev rules installed successfully: %s\n", udev_rules_path);
    printf("Please reload udev rules by running:\nsudo udevadm control --reload-rules && sudo udevadm trigger\n");
}
