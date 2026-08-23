#include <epicsThread.h>
#include <epicsExit.h>
#include <iocsh.h>

int main(int argc, char *argv[])
{
    if (argc >= 2) {
        iocsh(argv[1]);

        // Keep the IOC alive and provide an interactive IOC shell.
        iocsh(NULL);

        epicsExitCallAtExits();
        return 0;
    }

    iocsh(NULL);
    epicsExitCallAtExits();
    return 0;
}
