# yap Workspace Instructions

- When performing a complete build for this repository, increment the build number before building.
- Treat a complete build as any end-to-end build intended to produce the repository's normal distributable outputs, especially `make`, `make static`, `make dynamic`, or `make package`.
- Do not change the build number for partial validation steps such as syntax-only checks, focused compile tests, or other non-release verification commands.
- Ensure the build number is included in the package version consumed by dimsim before running `dpkbuild build pkg/`.
- The dimsim package version source is `pkg/meta/manifest.json`, so update its `version` value as part of the build-number bump when packaging.
- If a build-number change is made for a complete build, mention the new build number and resulting package version in the final response.