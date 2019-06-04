#include <stdio.h>
#include <syscall.h>
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "devices/timer.h"
#include "devices/rtc.h"

int
main (int argc, char **argv)
{
  int i;
  int j;

  for (i = 0; i < argc; i++){
    printf ("%s ", argv[i]);
  }
  printf ("\n");
  
  
    int arr[100000000] = {0};        
    int arrsize = 100000000;
    char arr2[100000000] = {0};
    float arr3[100000000] = {0};

  
  int** pArr;
  pArr = (int**)malloc(sizeof(int*)*(arrsize));
  pArr[arrsize] = 0;

  char** pArr2;
  pArr2 = (char**)malloc(sizeof(char*)*(arrsize));
  pArr2[arrsize] = 0;

  float* pArr3;
  pArr3 = (float*)malloc(sizeof(float)*(arrsize));
  pArr3[arrsize] = 0;


  
    for (j = 0; j < arrsize; j++)
    {
        pArr[j] = arr[j];
        pArr2[j] = arr2[j];
        pArr3[j] = arr3[j];
    }
  
  return EXIT_SUCCESS;
}

