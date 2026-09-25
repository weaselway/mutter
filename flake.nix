{
  description = "mutter (weaselway RDP backend) dev environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
  };

  outputs =
    { self, nixpkgs }:
    let
      forAllSystems =
        f:
        nixpkgs.lib.genAttrs [ "x86_64-linux" "aarch64-linux" ] (
          system: f nixpkgs.legacyPackages.${system}
        );
    in
    {
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          # Full build closure of nixpkgs' own mutter, which is the same release
          # line as ours.
          inputsFrom = [ pkgs.mutter ];

          packages = with pkgs; [
            # -Drdp=enabled: freerdp3, freerdp-server3, winpr3. nixpkgs' mutter
            # is built without the RDP backend, so it is not in inputsFrom.
            freerdp
            pipewire

            meson
            ninja
            pkg-config
            gdb
            git
          ];

          # The meson flags live in ./weaselway-build.sh, see WEASELWAY.md.
          shellHook = ''
            echo "build with: ./weaselway-build.sh"
          '';
        };
      });
    };
}
