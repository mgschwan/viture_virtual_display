{
  pkgs,
  self,
  stdenv,
  lib,
  freeglut,
  gdk-pixbuf,
  glfw,
  glib,
  glm,
  hidapi,
  libGL,
  libGLU,
  libusb1,
  libv4l,
  pipewire,
  pkg-config,
  withVitureSdk ? false,
}:
stdenv.mkDerivation {
  pname = "viture_virtual_display";
  version = "git";

  src = lib.cleanSource self;

  nativeBuildInputs = [
    pkg-config
  ];

  buildInputs = [
    freeglut
    gdk-pixbuf
    glfw
    glib
    glm
    hidapi
    libGL
    libGLU
    libusb1
    libv4l
    pipewire
  ];

  makeFlags = lib.optionalString withVitureSdk "viture_sdk";

  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin
    cp ${
      if withVitureSdk
      then "v4l2_gl_viture_sdk"
      else "v4l2_gl"
    } $out/bin
    runHook postInstall
  '';

  meta = {
    mainProgram =
      if withVitureSdk
      then "v4l2_gl_viture_sdk"
      else "v4l2_gl";
    maintainers = with lib.maintainers; [shymega];
  };
}
