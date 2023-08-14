FROM ubuntu:20.04

# (1) Dependencies
# ----------------------------------------

# Disable Prompt During Packages Installation
ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get -y update && \
    apt-get -y install --no-install-suggests --no-install-recommends \
    wget \
    git  \
    build-essential \
    cmake \
    ninja-build \
    python3 \ 
    zip \
    unzip \
    vim \ 
    mlocate \
    clang-format

# OpenFST doesn't seem to be packaged for Ubuntu (SFST is..)
RUN cd /tmp && \
    wget -nv --no-check-certificate http://www.openfst.org/twiki/pub/FST/FstDownload/openfst-1.6.9.tar.gz && \
    tar xf openfst-1.6.9.tar.gz && \
    cd openfst-1.6.9 && ./configure && make -j10 && make install
  # sha256=de5959c0c7decd920068aa4f9405769842b955719d857fd5d858fcacf0998bda

RUN mkdir /build
WORKDIR /build

#set LLVM version, only tested with LLVM 8 rn
ENV LLVM_PROJECT_VERSION llvmorg-8.0.0

#get a stable llvm version
RUN cd /tmp && wget --no-check-certificate --progress=dot:giga https://github.com/llvm/llvm-project/archive/refs/tags/${LLVM_PROJECT_VERSION}.zip && \
    unzip ${LLVM_PROJECT_VERSION}.zip && mv llvm-project-${LLVM_PROJECT_VERSION} /build/llvm

#Get opencilk binaries 
RUN wget --no-check-certificate \
       https://github.com/OpenCilk/opencilk-project/releases/download/opencilk%2Fv2.0/OpenCilk-2.0.0-x86_64-Linux-Ubuntu-20.04.sh 

RUN chmod u+x OpenCilk-2.0.0-x86_64-Linux-Ubuntu-20.04.sh

RUN mkdir /build/opencilk

RUN ./OpenCilk-2.0.0-x86_64-Linux-Ubuntu-20.04.sh --skip-license --prefix=/build/opencilk

# (2) Orchard
# ----------------------------------------

ADD orchard /build/llvm/clang/tools/orchard

ADD orchard-examples /build/orchard-examples

RUN echo 'add_clang_subdirectory(orchard)' >> /build/llvm/clang/tools/CMakeLists.txt  

RUN echo 'export LD_LIBRARY_PATH="/usr/local/lib"' >> ~/.bashrc

RUN echo 'export PATH=$PATH:"/build/llvm/build/bin"' >> ~/.bashrc

RUN echo 'export PATH=$PATH:"/build/opencilk/bin"' >> ~/.bashrc

RUN mkdir /build/llvm/build && cd llvm/build && \
    cmake -G Ninja ../llvm -DCMAKE_BUILD_TYPE=Debug -DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra" && \
    ninja orchard -j10
    
CMD ["bash"]
