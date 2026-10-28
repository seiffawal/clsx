clsx GitHub Actions build setup
================================

1. Put .github/workflows/build.yml in the root of your GitHub repository.
2. Ensure the C source file is named clsx.c and is in the repository root.
3. Push the workflow to GitHub. Open the Actions tab to see the build.
4. For a downloadable build, open a successful workflow run and download the
   clsx-windows-x64 artifact.
5. To publish release assets, push a version tag, for example:
       git tag v1.0.0
       git push origin v1.0.0
   The workflow builds clsx.exe and attaches it plus clsx.exe.sha256 to the
   GitHub Release for that tag.

Trust notes
-----------
- The workflow uses GitHub-hosted Windows runners and records build provenance.
- The SHA-256 file lets users check that a downloaded file matches the published
  checksum; by itself it does not prove the binary is safe or authentic.
- Build provenance lets users inspect which repository revision and workflow
  produced the artifact. Users should verify the attestation before trusting it.
- This does NOT code-sign the executable. Windows SmartScreen or antivirus may
  still warn about a new or unsigned program. Do not promise that this eliminates
  antivirus detections.
- The workflow assumes the source file is named clsx.c in the repository root.
