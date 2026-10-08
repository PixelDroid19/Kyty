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
        # Build and test toolchain for the C++ emulator and the Python tools.
        default = pkgs.mkShell {
          packages = with pkgs; [
            cmake
            ninja
            pkg-config
            gcc
            python3
            gdb
            SDL2
            vulkan-loader
            vulkan-headers
            spirv-tools
            glslang
            ffmpeg
          ];
        };

        # Adds the public ROCm HIP tools for the optional hardware arithmetic oracle.
        # Running the oracle still needs an AMD GPU exposed through /dev/kfd.
        oracle = pkgs.mkShell {
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
