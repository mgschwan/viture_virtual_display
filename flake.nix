{
  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-unstable";
  };

  outputs = inputs: let
    inherit (inputs) self nixpkgs;

    forEachSystem = let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      genPkgs = system:
        import nixpkgs {
          inherit system;
        };

      inherit (nixpkgs.lib) genAttrs;
    in
      f: genAttrs systems (system: f {pkgs = genPkgs system;});
  in {
    packages = forEachSystem ({pkgs}: {
      default = self.packages.${pkgs.system}.v4l2_gl;
      v4l2_gl = pkgs.callPackage ./build-aux/nix/default.nix {inherit self;};
      v4l2_gl_viture = pkgs.callPackage ./build-aux/nix/default.nix {
        inherit self;
        withVitureSdk = true;
      };
    });

    devShells = forEachSystem ({pkgs}: {
      default = pkgs.mkShell {
        inputsFrom = with self.packages.${pkgs.system}; [
          v4l2_gl
          v4l2_gl_viture
        ];
      };
    });
  };
}
