{
  description = "Kyty development environment, pinned to one nixpkgs commit";

  # Exact commit, not a branch. Change it only by a reviewed commit that
  # re-verifies every attribute listed in docs/devtools/reproducible-contract-environment.md.
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/151fa4e8ddfdd8dd25d945ad94ed54a13de9f6e4";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
    in
    {
      devShells.${system} = {
        # Build and test toolchain for the C++ emulator and its tools.
        default = pkgs.mkShell {
          # The project sets its own warning flags; the wrapper's format hardening
          # turns the vendored profiler's -Wno-format into an error.
          hardeningDisable = [ "format" ];
          packages = with pkgs; [
            cmake
            ninja
            pkg-config
            gcc
            git
            vulkan-loader
            vulkan-headers
            spirv-tools
            shaderc
            glslang
            ffmpeg
            alsa-lib
            libpulseaudio
            xorg.libX11
            xorg.libXext
            xorg.libXrandr
            xorg.libXcursor
            xorg.libXi
            xorg.libXfixes
            xorg.libXScrnSaver
            xorg.libXtst
            xorg.libxcb
          ];
        };

        # Adds Qt 6 for the desktop launcher.
        launcher = pkgs.mkShell {
          hardeningDisable = [ "format" ];
          inputsFrom = [ self.devShells.${system}.default ];
          packages = with pkgs.qt6; [
            qtbase
            qtdeclarative
            qttools
          ];
        };

        # Adds the public ROCm HIP tools for the optional hardware arithmetic oracle.
        # Running the oracle still needs an AMD GPU exposed through /dev/kfd.
        oracle = pkgs.mkShell {
          hardeningDisable = [ "format" ];
          inputsFrom = [ self.devShells.${system}.default ];
          packages = with pkgs.rocmPackages; [
            hipcc
            clr
            rocminfo
          ];
        };
      };
    };
}
