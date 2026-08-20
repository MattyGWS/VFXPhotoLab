#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright Contributors to the OpenColorIO Project.

# For OS X
export DYLD_LIBRARY_PATH="/home/mattyws/Documents/GitHub/VFXPhotoLab/third_party/opencolorio/lib64:${DYLD_LIBRARY_PATH}"

# For Linux
export LD_LIBRARY_PATH="/home/mattyws/Documents/GitHub/VFXPhotoLab/third_party/opencolorio/lib64:${LD_LIBRARY_PATH}"

export PATH="/home/mattyws/Documents/GitHub/VFXPhotoLab/third_party/opencolorio/bin:${PATH}"
export PYTHONPATH="/home/mattyws/Documents/GitHub/VFXPhotoLab/third_party/opencolorio/:${PYTHONPATH}"
