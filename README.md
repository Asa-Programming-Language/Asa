<div align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://github.com/sam-astro/ASA/blob/main/media/ASA-Full.png?raw=true">
    <source media="(prefers-color-scheme: light)" srcset="https://github.com/sam-astro/ASA/blob/main/media/ASA-Full-light.png?raw=true">
    <img class="portfolio" src="https://raw.githubusercontent.com/sam-astro/ASA/main/media/ASA-Full-light.png" width="60%" alt="Asa Programming Language" >
  </picture>
</div>

[![Badge License]][License]   ![Relative date](https://img.shields.io/date/1920814400?label=release%20timeline&color=purple)   ![GitHub commits difference between two branches/tags/commits](https://img.shields.io/github/commits-difference/sam-astro/Asa?base=main&head=dev&label=commits%20behind%20dev&color=orange)  ![Dynamic Regex Badge](https://img.shields.io/badge/dynamic/regex?url=https%3A%2F%2Fraw.githubusercontent.com%2FAsa-Programming-Language%2FAsa%2Frefs%2Fheads%2Fdev%2Fbuild_num&search=.*&label=build%20num)
   [![Button Discord]][Discord Server] 

---

This repository contains all of the source code for the Asa programming language compiler and standard libraries.

> There are no releases currently, and much of the current code is subject to change

## Building from source
This method has only been tested on Debian Linux version 12.2, but may be applicable to other versions and operating systems.

1. **Clone the repository**

    ```bash
    git clone --depth=1 https://github.com/Asa-Programming-Language/Asa
    ```

2. **Install required packages**

   ```bash
   # CMake version 3.25 is the only tested version
   sudo apt-get install -y cmake libedit-dev #TODO: update with new required packages
   ```

3. **Clone and build LLVM from source**

   ```bash
   # Get LLVM and switch to version 21.1.0
   git clone https://github.com/llvm/llvm-project;
   cd llvm-project;
   git checkout 3623fe6;
   ```
   Build:
   > It is recommended to use Ninja to build LLVM, since it saves time. ***If you don't have ninja installed,*** you can install it with: `sudo apt-get install ninja`. Or, if you prefer to use make, you can replace `Ninja` below with: `"Unix Makefiles"` (including quotes).
   ```bash
   cmake -S llvm -B build -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi" -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_PARALLEL_LINK_JOBS=2 -DLLVM_USE_LINKER=lld -DLLVM_ENABLE_RUNTIMES="libunwind"; # This command builds llvm, but only the components required by Asa. This saves time and space
   cd build;
   cmake --build .;
   sudo cmake --install .;
   ```

4. **Build and run ASA**

   Enter ASA primary directory, then run the following to build:
   
   ```bash
   ./src/build.sh
   ```

   You may also run `./src/run.sh`, which will build ASA, but then also run it. this is useful for fast development testing.

   The built executable will be located at `ASA/build/asa`

<!----------------------------------------------------------------------------->

[License]: LICENSE
[Discord Server]: https://discord.gg/9p82dTEdkN

<!----------------------------------[ Badges ]--------------------------------->

[Badge License]: https://img.shields.io/github/license/sam-astro/ASA
[Button Discord]: https://img.shields.io/badge/Discord_Server-573f75.svg?style=social&logo=Discord
