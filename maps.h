#include <stdint.h>

#ifndef MAP_H
#define MAP_H


#define MAP_SMALL 10
#define MAP_MEDIUM 25
#define MAP_LARGE 50

#define TILE_SIZE = 25;	 //Arbitrary unit used for position tracking

typedef struct {
	int size; 	 // Width and height
	int tiles[MAP_LARGE][MAP_LARGE]; 
} map_t;

extern map_t map_small_1;
extern map_t map_medium_1;
extern map_t map_large_1;


#endif
