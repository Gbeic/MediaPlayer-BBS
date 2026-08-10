# MediaPlayer

## A decode library for Minecraft based on [ffmpeg](https://ffmpeg.org/).

### Features:

* Video and audio decoder
* Hardware acceleration
* Supports many media formats
* Only Windows x64 platform is supported

### How to use:

#### For user

Use the commands

```
/summon mediaplayer:video
/mediaplayer video
```

to play video.<br><br>
Or use

```
/mediaplayer audio
```

to play audio.<br><br>
Use

```
/mediaplayer clear
```

stop all video and audio.

#### For developer

Create decoder

```java
var file = new File("<video file path>");
var decoder = VideoDecoder.create(file);
```

Decode and render frame

```java
decoder.fetch();
bufferSource.getBuffer(VideoRenderType.create(decoder.frame));
/* Rendering code */
```

Play companion audio from video

```java
Minecraft.getInstance().getSoundManager().play(new AudioInstance(decoder.audio.decode(false),null));
```

Play independent audio

```java
Minecraft.getInstance().getSoundManager().play(new AudioInstance(new AudioDecoder.decode(false),null));
```

## Building the Native Backend with GitHub Actions

The `.github/workflows/build-native.yml` workflow prepares JDK, MSVC, FFmpeg LGPL shared development files, CUDA Toolkit, `nv-codec-headers`, and DXC on a Windows x64 runner, then builds the complete native backend.

To use it:

1. Open the repository's `Actions` page and select the workflow for building the complete MediaPlayer-BBS native backend.
2. Click `Run workflow` and wait for the Windows build to finish.
3. Download `MediaPlayer-native-windows-x64` from the run's artifacts.
4. Extract the archive and place its DLL files and `BUILD-MANIFEST.txt` in `common/src/main/resources`, then rebuild the mod.

This workflow does not depend on a local FFmpeg development package, CUDA installation path, or Visual Studio installation path. GitHub-hosted runners do not provide an NVIDIA GPU for runtime validation, so a successful build confirms that the CUDA/D3D code compiled and linked, but does not prove that hardware decoding ran in the cloud. Test the downloaded files on a machine with the target GPU.

`build-native-software.ps1` remains available for legacy software-only environments. The normal complete native build should use `build-native.ps1` or GitHub Actions.
