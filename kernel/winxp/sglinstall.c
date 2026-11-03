/*
 * Installs sgldrv.inf (next to this program) for the ivshmem PCI device.
 * XP asks for confirmation because the driver is unsigned: "Continue Anyway".
 */
#include <windows.h>
#include <newdev.h>
#include <stdio.h>

int main(void)
{
    char path[MAX_PATH];
    char *slash;
    BOOL reboot = FALSE;

    GetModuleFileNameA(NULL, path, sizeof(path));
    slash = strrchr(path, '\\');
    if (slash == NULL)
        return 2;
    strcpy(slash + 1, "sgldrv.inf");

    printf("Installing %s\n", path);
    if (!UpdateDriverForPlugAndPlayDevicesA(NULL, "PCI\\VEN_1AF4&DEV_1110", path, INSTALLFLAG_FORCE, &reboot)) {
        DWORD error = GetLastError();
        printf("FAILED: error %lu (0x%08lx)\n", error, error);
        if (error == ERROR_NO_SUCH_DEVINST)
            printf("This VM has no ivshmem device. Start it with the SharedGL script.\n");
        return 1;
    }

    printf("Installed.%s\n", reboot ? " Restart Windows to finish." : "");
    return 0;
}
