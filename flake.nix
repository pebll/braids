# flake.nix
#
# Copyright (c) 2026 braids contributors
#
# braids is distributed under the GNU AGPL v3 license, see LICENSE for details.
#
# braids for Nix users (the latest release, see nix/package.nix):
#
#   nix run github:pebll/braids
#   nix profile install github:pebll/braids
#
# NixOS: add the input and `inputs.braids.packages.${pkgs.system}.default`
# to environment.systemPackages (or home.packages).

{
  description = "braids: Telegram, WhatsApp, Signal and Matrix in one desktop app";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAll (pkgs: rec {
        braids = pkgs.callPackage ./nix/package.nix { };
        default = braids;
      });

      apps = forAll (pkgs: rec {
        braids = {
          type = "app";
          program = "${self.packages.${pkgs.stdenv.hostPlatform.system}.braids}/bin/braids";
        };
        default = braids;
      });

      overlays.default = final: prev: {
        braids = final.callPackage ./nix/package.nix { };
      };
    };
}
