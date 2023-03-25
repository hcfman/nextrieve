
#include <stdio.h>

int main(int argc, char **argv)
{
    int x;
    unsigned char *c;

    x = 1;
    c = (unsigned char *)&x;

    printf("0x%x 0x%x 0x%x 0x%x\n", *(c+0), *(c+1), *(c+2), *(c+3));
    return 0;
}
