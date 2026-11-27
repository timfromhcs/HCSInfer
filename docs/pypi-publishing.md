# Publishing hcsinfer to PyPI (owner steps)

Nothing publishes automatically. `.github/workflows/pypi.yml` starts only from a manual `workflow_dispatch` or from a
published GitHub release, never from a tag push. It uses PyPI Trusted Publishing (OIDC, no API token is stored) with
attestations enabled, through `pypa/gh-action-pypi-publish` pinned to a full commit SHA (v1.14.3).

## One-time setup (owner)

1. Create the project name on both indexes (the first upload can be a "pending publisher"):
   * https://pypi.org/manage/account/publishing/ and https://test.pypi.org/manage/account/publishing/
2. Add a trusted publisher on each with: owner `timfromhcs`, repository `HCSInfer`, workflow file `pypi.yml`, and
   environment name `pypi` (PyPI) / `testpypi` (TestPyPI). Project name: `hcsinfer`.
3. In the GitHub repository settings, create the environments `pypi` and `testpypi`. Add required reviewers to `pypi`
   (and ideally restrict it to the release branch/tags). No secrets are needed.
4. Confirm the version in `python/pyproject.toml` (the coordinator bumps `1.1.0rc1` to the release version at release
   time) and that the 2FA / account security settings of the PyPI owner are enabled.

## Publishing a release

1. Run `wheels.yml` (via the release build, or manually) so that `wheel-<plat>`, `wheel-pure` and `sdist` artifacts exist.
   Hosted Linux and macOS wheel jobs are NOT RUN until this has been done once on GitHub.
2. Rehearse: dispatch `pypi.yml` with `repository: testpypi` and `wheels-run-id: <run id of wheels.yml>`; then
   `pip install --index-url https://test.pypi.org/simple/ --no-deps hcsinfer==<version>` in a clean venv and run
   `hcsinfer --version`.
3. Publish: dispatch `pypi.yml` with `repository: pypi` and the same run id and approve the `pypi` environment. Or attach
   the wheels and sdist to a GitHub release and publish the release: the workflow then downloads those release assets and
   uploads them to PyPI (it fails closed when there are none).
4. PyPI uploads are immutable: a wrong file means a new version (yank the bad one), so always rehearse on TestPyPI first.

The workflow runs `twine check --strict` on everything before the environment gate, and refuses to continue without
distributions. It cannot be triggered by a pull request.
