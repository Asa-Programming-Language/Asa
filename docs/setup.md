%%% order 0

# Setup

!!! warning: There are no releases currently, and much of the current code is subject to change
## Building from source
This method has only been tested on Debian Linux version 12.2, but may be applicable to other versions and operating systems.

1. **Clone the repository**

    ```!shell
    $ git clone --depth=1 https://github.com/Asa-Programming-Language/Asa
    ```

2. **Install required packages**
    ```!shell
    $ # CMake version 3.25 is the only tested version
    $ sudo apt-get install -y cmake libedit-dev
    ```
    TODO: update with new required packages

3. **Clone and build LLVM from source**

    ```!shell
    $ # Get LLVM and switch to version 21.1.0
    $ git clone https://github.com/llvm/llvm-project;
    $ cd llvm-project;
    $ git checkout 3623fe6;
    ```
    Build:

    !!! note: It is recommended to use Ninja to build LLVM, since it saves time. ***If you don't have ninja installed,*** you can install it with: `sudo apt-get install ninja`. Or, if you prefer to use make, you can replace `Ninja` below with: `"Unix Makefiles"` (including quotes).
    ```!shell
    $ # This command builds llvm, but only the components required by Asa. This saves time and space
    $ cmake -S llvm -B build -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi" \
        -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_PARALLEL_LINK_JOBS=2 -DLLVM_USE_LINKER=lld \
        -DLLVM_ENABLE_RUNTIMES="libunwind";
    $ cd build;
    $ cmake --build .;
    $ sudo cmake --install .;
    ```

4. **Build and run Asa**

    Enter Asa primary directory, then run the following to build:
    
    ```!shell
    $ ./src/build.sh
    ```
 
    You may also run `./src/run.sh`, which will build Asa, but then also run it and its tests. This is useful for fast development testing.
 
    The built executable will be located at `Asa/build/asa`

## Now, if you are new to Asa, lets head to [Introduction to Asa](introduction.html)
