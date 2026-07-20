#include <stdio.h>
#include <switch.h>

static const char* appletTypeName(AppletType type)
{
    switch (type)
    {
        case AppletType_None:             return "None";
        case AppletType_Default:          return "Default";
        case AppletType_Application:      return "Application";
        case AppletType_SystemApplet:     return "SystemApplet";
        case AppletType_LibraryApplet:    return "LibraryApplet";
        case AppletType_OverlayApplet:    return "OverlayApplet";
        case AppletType_SystemApplication: return "SystemApplication";
        default:                          return "Unknown";
    }
}

static const char* buttonName(u64 button)
{
    switch (button)
    {
        case HidNpadButton_A:       return "A";
        case HidNpadButton_B:       return "B";
        case HidNpadButton_X:       return "X";
        case HidNpadButton_Y:       return "Y";
        case HidNpadButton_Plus:    return "PLUS";
        case HidNpadButton_Minus:   return "MINUS";
        case HidNpadButton_Up:      return "UP";
        case HidNpadButton_Down:    return "DOWN";
        case HidNpadButton_Left:    return "LEFT";
        case HidNpadButton_Right:   return "RIGHT";
        case HidNpadButton_L:       return "L";
        case HidNpadButton_R:       return "R";
        case HidNpadButton_ZL:      return "ZL";
        case HidNpadButton_ZR:      return "ZR";
        case HidNpadButton_StickL:  return "L3";
        case HidNpadButton_StickR:  return "R3";
        default:                    return "?";
    }
}

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    consoleInit(NULL);

    /* Reconfigure input at runtime */
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    printf("\x1b[2J\x1b[H");
    printf("=== SMOKE INPUT V5 ===\n\n");

    printf("Applet type: %s (%d)\n",
           appletTypeName(appletGetAppletType()),
           (int)appletGetAppletType());

    SetSysFirmwareVersion fw;
    if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
        printf("Firmware: %u.%u.%u\n", fw.major, fw.minor, fw.micro);

    printf("Operation mode: %s\n",
           appletGetOperationMode() == AppletOperationMode_Handheld
               ? "Handheld" : "Docked");
    printf("\nPress any key. B or PLUS to exit.\n");

    consoleUpdate(NULL);

    /* Loop with 15s timeout */
    u64 startTick = armGetSystemTick();
    while (appletMainLoop())
    {
        padUpdate(&pad);
        u64 pressed = padGetButtonsDown(&pad);

        if (pressed)
        {
            printf("BUTTON:");
            for (int i = 0; i < 64; i++)
            {
                u64 mask = (u64)1 << i;
                if (pressed & mask)
                    printf(" %s", buttonName(mask));
            }
            printf("\n");
            consoleUpdate(NULL);

            if (pressed & (HidNpadButton_Plus | HidNpadButton_B))
            {
                printf("Exit requested.\n");
                consoleUpdate(NULL);
                break;
            }
        }

        /* 15 second timeout */
        if (armTicksToNs(armGetSystemTick() - startTick) > 15000000000ULL)
        {
            printf("Timeout (15s). Exiting.\n");
            consoleUpdate(NULL);
            break;
        }

        consoleUpdate(NULL);
    }

    consoleExit(NULL);
    return 0;
}
