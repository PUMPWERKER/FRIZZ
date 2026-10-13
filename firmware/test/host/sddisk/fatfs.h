// sddisk.cpp's fatfs.h: FatFs itself (libDaisy's, vendored), in place of host/fatfs.h's card in
// memory, so SceneStore.h runs on the real FatFs
#pragma once
#include "ff.h"
