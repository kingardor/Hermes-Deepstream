APP:= hermes-app

CXX:= g++ -std=c++17

TARGET_DEVICE = $(shell g++ -dumpmachine | cut -f1 -d -)

NVDS_VERSION:=5.1

LIB_INSTALL_DIR?=/opt/nvidia/deepstream/deepstream-$(NVDS_VERSION)/lib/

ifeq ($(TARGET_DEVICE),aarch64)
  CFLAGS:= -DPLATFORM_TEGRA
endif

SRCS:= $(wildcard ds_src/*.c)
SRCS+= $(wildcard ds_src/*.cpp)

INCS:= $(wildcard ds_src/*.h)

PKGS:= gstreamer-1.0 gstreamer-rtsp-server-1.0

OBJS:= $(SRCS:.cpp=.o)

CFLAGS+= -I/opt/nvidia/deepstream/deepstream-5.1/sources/includes \
		 -DDS_VERSION_MINOR=0 -DDS_VERSION_MAJOR=5

CFLAGS+= `pkg-config --cflags $(PKGS)`

LIBS:= `pkg-config --libs $(PKGS)`

LIBS+= -L$(LIB_INSTALL_DIR) -L/usr/local/cuda/lib64 -lcudart \
	   -lnvdsgst_meta -lnvds_meta -lnvdsgst_helper -lm -lrt \
       -Wl,-rpath,$(LIB_INSTALL_DIR)

LIBS+= -pthread -O3 -Ofast

LIBS+= -lhiredis -luuid -lnvbufsurface -lnvbufsurftransform

LIBS+= -lboost_system -lboost_filesystem

all: hermes objdets

objdets: yolov3
hermes: $(APP)

%.o: %.cpp $(INCS) Makefile
	$(CXX) -c -o $@ $(CFLAGS) $<

$(APP): $(OBJS) Makefile
	$(CXX) -o $(APP) $(OBJS) $(LIBS)

yolov3:
	cd custom_parsers/nvds_customparser_yolov3 && $(MAKE)

clean:
	rm -rf $(OBJS) $(APP)
	cd custom_parsers/nvds_customparser_yolov3 && $(MAKE) clean