#include "doomgeneric.h"

#include <fs/file.h>
#include <stdio.h>
#include <stdbool.h>

int main(int argc, char **argv)
{

    doomgeneric_Create(argc, argv);

    while (1)
    {
        doomgeneric_Tick();
    }
    
    return 0;
}