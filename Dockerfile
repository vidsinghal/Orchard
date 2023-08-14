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

# gcc g++ automake autoconf make 

# OpenFST doesn't seem to be packaged for Ubuntu (SFST is..)
RUN cd /tmp && \
    wget -nv --no-check-certificate http://www.openfst.org/twiki/pub/FST/FstDownload/openfst-1.6.9.tar.gz && \
    tar xf openfst-1.6.9.tar.gz && \
    cd openfst-1.6.9 && ./configure && make -j10 && make install
  # sha256=de5959c0c7decd920068aa4f9405769842b955719d857fd5d858fcacf0998bda

RUN mkdir /build
WORKDIR /build

# Current Master on both [2018.11.04]:
#ENV LLVM_SHA  97d7bcd5c024ee6aec4eecbc723bb6d4f4c3dc3d
#ENV CLANG_SHA 6093fea79d46ed6f9846e7f069317ae996149c69

#set LLVM version, only tested with LLVM 8 rn
ENV LLVM_PROJECT_VERSION llvmorg-8.0.0

# https://git.llvm.org/git/llvm.git/
# https://github.com/llvm-mirror/llvm
# https://github.com/llvm-mirror/clang

# Fetching llvm/clang -
# # OPTION 1:
# # Problem here - this seems to sometimes work and sometimes not
# #  ("does not allow request for unadvertised object")!
# #
# # Git doesn't support direct checkout of a commit:
# RUN mkdir llvm && cd llvm && git init && \
#     git remote add origin https://github.com/llvm-mirror/llvm && \
#     git fetch --depth 1 origin ${LLVM_SHA} && git checkout FETCH_HEAD
# RUN cd llvm/tools && mkdir clang && cd clang && git init && \
#    git remote add origin https://github.com/llvm-mirror/clang && \
#    git fetch --depth 1 origin ${CLANG_SHA} && git checkout FETCH_HEAD

# OPTION 2:  Download tarballs.
#RUN cd /tmp && wget -nv --no-check-certificate https://github.com/llvm-mirror/llvm/archive/${LLVM_SHA}.tar.gz && \
#    tar xf ${LLVM_SHA}.tar.gz && mv llvm-${LLVM_SHA} /build/llvm

#get a stable llvm version
RUN cd /tmp && wget --no-check-certificate --progress=dot:giga https://github.com/llvm/llvm-project/archive/refs/tags/${LLVM_PROJECT_VERSION}.zip && \
    unzip ${LLVM_PROJECT_VERSION}.zip && mv llvm-project-${LLVM_PROJECT_VERSION} /build/llvm


# get opencilk
RUN cd /tmp && wget --no-check-certificate --progress=dot:giga https://github.com/OpenCilk/opencilk-project/archive/refs/tags/opencilk/v2.0.1.zip && \ 
	unzip v2.0.1.zip && mv opencilk-project-opencilk-v2.0.1 /build/. 

#build opencilk 
RUN mkdir /build/opencilk-project-opencilk-v2.0.1/build && cd opencilk-project-opencilk-v2.0.1/build && \
    cmake -G Ninja ../llvm -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra" && \
    ninja -j10



# ENV CLANG_TOOLS_EXTRA_SHA e936bbdce059a887ec69b05c8c701ff0c77d1f51
# #RUN apt-get install -y subversion
# # svn co http://llvm.org/svn/llvm-project/clang-tools-extra/trunk extra
# RUN cd llvm/tools/clang/tools && \
#     git clone https://git.llvm.org/git/clang-tools-extra.git && \
#     cd clang-tools-extra && \
#     git checkout ${CLANG_TOOLS_EXTRA_SHA}


# (2) Orchard
# ----------------------------------------

ADD orchard /build/llvm/clang/tools/orchard

ADD orchard-examples /build/orchard-examples

RUN echo 'add_clang_subdirectory(orchard)' >> /build/llvm/clang/tools/CMakeLists.txt  

RUN echo 'export LD_LIBRARY_PATH="/usr/local/lib"' >> ~/.bashrc

RUN echo 'export PATH=$PATH:"/build/llvm/build/bin"' >> ~/.bashrc

RUN echo 'export PATH=$PATH:"/build/opencilk-project-opencilk-v2.0.1/build/bin"' >> ~/.bashrc

# RUN apt-get install -y z3
# RUN apt-get install -y build-essential libc++-dev binutils

RUN mkdir /build/llvm/build && cd llvm/build && \
    cmake -G Ninja ../llvm -DCMAKE_BUILD_TYPE=Debug -DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra" && \
    ninja orchard -j10
    
CMD ["bash"]
