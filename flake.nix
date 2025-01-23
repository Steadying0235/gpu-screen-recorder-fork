{
  description = "Development environment for GPU Screen Recorder";

  inputs = {
    # Specify the Nixpkgs repository. You can pin to a specific commit or branch for stability.
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    
    # flake-utils provides helper functions for flakes.
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        # Import the appropriate packages for the target system.
        pkgs = import nixpkgs { inherit system; };
      in {
        # Define the development shell.
        devShell = pkgs.mkShell {
          # List of build and runtime dependencies.
          buildInputs = with pkgs; [
            # Build System
            meson
            ninja
            gcc

            # Graphics Libraries
            libglvnd
            vulkan-headers
            mesa

            # FFmpeg and related libraries
            ffmpeg
            libva
            libva-utils

            # X11 Libraries
            xorg.libX11
            xorg.libXcomposite
            xorg.libXrandr
            xorg.libXfixes
            xorg.libXdamage

            # PulseAudio
            pulseaudio

            # DRM and Capabilities
            libdrm
            libcap

            # Wayland Libraries
            wayland
            wayland-protocols
            egl-wayland
            xwayland

            # PulseAudio
            libpulseaudio

            # Additional Runtime Dependencies
            pipewire
            dbus
            pkg-config

          ];

           # For runtime linker to locate libraries like libEGL.so.1
          shellHook = ''
            export CXX=${pkgs.gcc}/bin/g++
            export CC=${pkgs.gcc}/bin/gcc
            export LD_LIBRARY_PATH=${pkgs.lib.makeLibraryPath [pkgs.mesa pkgs.libglvnd]}:$LD_LIBRARY_PATH
          '';
        };
      }
    );
}
