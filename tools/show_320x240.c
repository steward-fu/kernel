#include <stdio.h>
#include <stdlib.h>
#include <SDL.h>
#include <SDL_image.h>
#include <unistd.h> 
#include <fcntl.h>

int main(int argc, char** argv)
{
    SDL_Surface* screen=NULL;

    if(argc != 2){
        printf("usage:\n\tshow_320x240 xxx\n");
        return -1;
    }
 
    SDL_Init(SDL_INIT_VIDEO);
    screen = SDL_SetVideoMode(320, 240, 16, SDL_SWSURFACE);

    int fd = open(argv[1], O_RDONLY);
    if(fd > 0){
        read(fd, screen->pixels, 320*240*2);
        close(fd);

        SDL_Flip(screen);
        SDL_Event event;
        while(1){
            if(SDL_PollEvent(&event)){
                if(event.type == SDL_KEYDOWN){
                    break;
                }
            }
            usleep(100);
        }
    }
    SDL_Quit();
    return 0;
}
