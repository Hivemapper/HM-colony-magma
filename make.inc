# Hivemapper variant

# GPU_TARGET contains one or more of Fermi, Kepler, Maxwell, Pascal, Volta, Turing, or Ampere
# to specify for which GPUs you want to compile MAGMA:
#     Fermi          - NVIDIA compute capability 2.x cards
#     Kepler         - NVIDIA compute capability 3.x cards
#     Maxwell        - NVIDIA compute capability 5.x cards
#     Pascal         - NVIDIA compute capability 6.x cards
#     Volta/Turing   - NVIDIA compute capability 7.x cards
#     Ampere         - NVIDIA compute capability 8.x cards
# The default is "Kepler Maxwell Pascal".
# Note that NVIDIA no longer supports 1.x cards, as of CUDA 6.5.
# See http://developer.nvidia.com/cuda-gpus
#
GPU_TARGET ?= sm_52 sm_70 sm_75 sm_80 sm_86
BACKEND = cuda

# --------------------
# programs

# SEEME - bitweeder
# We use an explicit path for `nvcc` since it doesn’t get picked up otherwise.
# Relatedly, the mechanism for not building Fortran is opaque, so we set the
# binary to something silly. Note that `false` won’t work here since that will
# actually cause `make` to fail, so we go with something counterintuitive.
CC        = gcc
CXX       = g++
NVCC      = /usr/local/cuda/bin/nvcc
#FORT      = false

ARCH      = ar
ARCHFLAGS = cr
RANLIB    = ranlib


# --------------------
# flags

# Use -fPIC to make shared (.so) and static (.a) library;
# can be commented out if making only static library.
FPIC      = -fPIC

CFLAGS    = -O3 $(FPIC) -fopenmp -DNDEBUG -DADD_ -Wall -Wshadow -DMAGMA_NOAFFINITY
#CFLAGS    = -O3 $(FPIC) -fopenmp -DNDEBUG -DADD_ -Wall -Wno-strict-aliasing -Wshadow -DMAGMA_WITH_MKL
FFLAGS    = -O3 $(FPIC)          -DNDEBUG -DADD_ -Wall -Wno-unused-dummy-argument
NVCCFLAGS = -O3                  -DNDEBUG -DADD_ -Xcompiler "$(FPIC) -Wall -Wno-unused-function" -std=c++14
#NVCCFLAGS = -O3                  -DNDEBUG -DADD_ -Xcompiler "$(FPIC) -Wall -Wno-unused-function -Wno-strict-aliasing" -std=c++14
LDFLAGS   =     $(FPIC) -fopenmp

# Options to do extra checks for non-standard things like variable length arrays;
# it is safe to disable all these
CFLAGS   += -pedantic -Wno-long-long
#CFLAGS   += -Werror  # uncomment to ensure all warnings are dealt with

# C++11 (gcc >= 4.7) is not required, but has benefits like atomic operations
CXXFLAGS := $(CFLAGS) -std=c++14
CFLAGS   += -std=c99

prefix   = /usr/local


# --------------------
# libraries

# see MKL Link Advisor at http://software.intel.com/sites/products/mkl/
# gcc/gfortran with MKL 10.3, GNU OpenMP threads (use -fopenmp in CFLAGS, LDFLAGS)
# Supposedly, gcc can use Intel threads (libiomp5) instead, but be careful that
# libiomp5 and libgomp are NOT BOTH linked.
#LIB       = -lmkl_gf_lp64 -lmkl_gnu_thread -lmkl_core -lgomp -lpthread -lstdc++ -lm
#LIB       = -lmkl_gf_lp64 -lmkl_gnu_thread -lmkl_core -liomp5 -lpthread -lstdc++ -lm
LIB       = -lgomp -lpthread -lstdc++ -lm
LIB      += -lcublas -lcusparse -lcudart -lcudadevrt


# --------------------
# directories

# define library directories preferably in your environment, or here.
#MKLROOT ?= /opt/intel/composerxe/mkl
CUDADIR ?= /usr/local/cuda
#-include make.check-mkl
-include make.check-cuda

LIBDIR    = -L$(CUDADIR)/lib64
#LIBDIR    = -L$(CUDADIR)/lib64 \
#            -L$(MKLROOT)/lib/intel64

INC       = -I$(CUDADIR)/include
#INC       = -I$(CUDADIR)/include \
#            -I$(MKLROOT)/include
