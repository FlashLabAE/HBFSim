"""Open a persistent engine session by profile name, for custom policies.

The reference runner replays traces through fixed policies. To try your own
placement, caching, migration or prefetch idea, drive the engine directly:
build :class:`hbfsim_client.Transaction` DAGs in Python and submit them to a
:class:`hbfsim_client.SimulationSession`. :func:`open_session` does the
bookkeeping — resolving names, turning ``options`` into an overlay file, and
keeping wear reports inside the run directory::

    from hbfsim import open_session
    from hbfsim_client import Transaction

    with open_session("4hbm-4hbf", overlays=["ocp-v070-grade3"]) as session:
        batch = [Transaction(id=f"r{i}", target="HBF_LOGICAL", op="R",
                             addr=i * 4096, bytes=4096, issue_ns=0.0)
                 for i in range(1024)]
        print(session.run(batch).elapsed_ns)
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
from typing import Any, Mapping, Sequence

from hbfsim import catalog
from hbfsim.runner import prepare_output_directory
from hbfsim.workspace import ENGINE, HbfsimError, require_executable, repository_root
from hbfsim_client import ResolvedSystemConfig, SimulationSession


# The Python client derives HBF geometry from these keys; ratio-sized
# profiles leave some of them to the engine's resolver.
GEOMETRY_KEYS = (
    "hbf-stacks", "hbf-channels", "hbf-dies-per-channel", "hbf-planes-per-die",
    "hbf-blocks-per-plane", "hbf-pages-per-block", "hbf-page-size",
)

def _describe(engine: Path, files: Sequence[Path], *, full: bool = False) -> dict[str, Any]:
    command = [str(engine)]
    for path in files:
        command += ["--system-config", str(path)]
    command.append("--describe-system")
    completed = subprocess.run(command, capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        raise HbfsimError(
            "the engine rejected this configuration: "
            + (completed.stderr or completed.stdout).strip()
        )
    descriptor = json.loads(completed.stdout)
    return descriptor if full else dict(descriptor["values"])


def session_config(system: str | Path, overlays: Sequence[str | Path] = (),
                   options: Mapping[str, Any] | None = None, *,
                   out_dir: Path, frontend: str | None = None) -> ResolvedSystemConfig:
    """Resolve a system (+ overlays + key overrides) into a client config.

    Overrides are written to ``out_dir/overrides.cfg`` so the run directory
    records them. Profiles that size HBF by ratio get their resolved geometry
    written to ``out_dir/resolved-geometry.cfg``; the engine resolves the
    same values either way.
    """

    if frontend is not None and frontend not in ("legacy", "sharing-only", "page-mshr", "fine-v1"):
        raise HbfsimError(f"unknown frontend profile: {frontend}")
    files = [catalog.resolve("system", system)]
    profile = None
    if frontend not in (None, "legacy"):
        profile = catalog.resolve("overlay", f"hbf/frontend/{frontend}")
        files.append(profile)
    files += [catalog.resolve("overlay", overlay) for overlay in overlays]
    if options:
        overrides = out_dir / "overrides.cfg"
        overrides.write_text(
            "".join(f"{str(key).removeprefix('--')}={str(value).lower() if isinstance(value, bool) else value}\n" for key, value in options.items()),
            encoding="utf-8",
        )
        files.append(overrides)
    declared: dict[str, str] = {}
    for path in files:
        declared.update(catalog.parse_config(path))
    engine = require_executable(ENGINE)
    descriptor = _describe(engine, files, full=True)
    resolved = descriptor["values"]
    if profile and not set(catalog.parse_config(profile)).issubset(descriptor.get("frontend", {})):
        raise HbfsimError("engine lacks the requested frontend capabilities; rebuild HBFSim (no legacy fallback)")
    missing = {key: resolved[key] for key in GEOMETRY_KEYS
               if key in resolved and key not in declared}
    if missing:
        geometry = out_dir / "resolved-geometry.cfg"
        geometry.write_text(
            "# Geometry resolved by `hbfsim --describe-system` for the Python client.\n"
            + "".join(f"{key}={value}\n" for key, value in missing.items()),
            encoding="utf-8",
        )
        files.append(geometry)
    if frontend is not None:
        _write_frontend_receipt(out_dir, frontend, profile, files, declared, descriptor, engine)
    return ResolvedSystemConfig.load(tuple(files))


def open_session(system: str | Path = "4hbm-4hbf", overlays: Sequence[str | Path] = (),
                 options: Mapping[str, Any] | None = None, *,
                 out_dir: Path | None = None, overwrite: bool = False,
                 enable_hbm: bool = True, enable_hbf: bool = True,
                 enable_external: bool = False, frontend: str | None = None,
                 **session_options: Any) -> SimulationSession:
    """Start ``build/hbfsim`` for one configuration and return the session.

    Use it as a context manager. ``session_options`` are passed through to
    :class:`hbfsim_client.SimulationSession` (for example
    ``initial_hbf_logical_pages=N`` to start with N pages already written).
    """

    directory = prepare_output_directory(out_dir, "session", overwrite=overwrite)
    config = session_config(system, overlays, options, out_dir=directory, frontend=frontend)
    session_options.setdefault("hbf_wear_output_prefix", directory / "hbf-wear")
    return SimulationSession(
        simulator_path=require_executable(ENGINE),
        system_config=config,
        enable_hbm=enable_hbm,
        enable_hbf=enable_hbf,
        enable_external=enable_external,
        **session_options,
    )


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for part in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(part)
    return digest.hexdigest()


def _write_frontend_receipt(directory: Path, name: str, profile: Path | None,
                            files: Sequence[Path], declared: Mapping[str, str],
                            descriptor: Mapping[str, Any], engine: Path) -> None:
    # The executable hash is authoritative; source identity describes this
    # checkout and never asserts that an arbitrary external binary was built here.
    root = repository_root()
    def git(*args: str) -> str | None:
        result = subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True)
        return result.stdout.strip() if result.returncode == 0 else None
    overrides = {}
    if profile:
        for key, value in catalog.parse_config(profile).items():
            if declared.get(key, "").lower() != value.lower():
                overrides[key] = declared.get(key)
    receipt = {
        "schema": {"name": "hbfsim.frontend_configuration", "version": 1},
        "profile": name, "profile_version": 1,
        "status": "custom-derived" if overrides else "frozen" if name == "fine-v1" else "reference",
        "classification": "custom-derived" if overrides else name,
        "profile_sha256": _sha256(profile) if profile else None,
        "profile_overrides": overrides,
        "configuration_order": [{"path": str(p), "sha256": _sha256(p),
                                  "values": catalog.parse_config(p)} for p in files],
        "declared_values": dict(declared), "resolved_system": descriptor,
        "engine": {"path": str(engine), "sha256": _sha256(engine)},
        "source_checkout": {"path": str(root), "commit": git("rev-parse", "HEAD"),
                            "status": git("status", "--porcelain"),
                            "tracked_diff_sha256": hashlib.sha256((git("diff", "HEAD") or "").encode()).hexdigest()},
        "admission_assumption": "open-loop within each submitted batch; no inferred GPU inflight cap",
        "drain_policy": "serving timing excludes terminal drain; session close drains separately",
        "history_memory": "payload peak excludes hash/vector metadata and allocator overhead; measure process RSS separately",
    }
    (directory / "frontend-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")


def open_fine_session(system: str | Path = "4hbm-4hbf", overlays: Sequence[str | Path] = (),
                      options: Mapping[str, Any] | None = None, *,
                      frontend: str = "fine-v1", **session_options: Any) -> SimulationSession:
    """Open a fine-trace session with an explicit, overridable frontend policy.

    Order: system, frontend, user overlays, explicit options, resolved geometry.
    ``fine-v1`` is the frozen v1 baseline; configuration assumptions are recorded in
    frontend-receipt.json. Select ``legacy`` to inject no frontend configuration.
    All transaction encodings use the same device policy, including coarse inputs.
    """
    return open_session(system, overlays, options, frontend=frontend, **session_options)
