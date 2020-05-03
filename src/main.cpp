#include <Windows.h>

#include <km_common/km_defines.h>

void UseUpThatStackSpace()
{
    uint8 buffer[4096];
    int i = 0;
    while (i != 109238) {
        buffer[i % 4096] = (char)(i % 256);
    }
}

int CALLBACK WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
    int hello[1024] = {0};
    
    UseUpThatStackSpace();
    
    float f = 1.0f;
    float g = f + 1.0f;
    
    ExitProcess(0);
}
