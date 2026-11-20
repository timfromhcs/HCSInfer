"""Console script ``hcsinfer``: forwards every argument to the bundled HCSInfer executable."""
from __future__ import annotations

import sys
from typing import Optional, Sequence

from ._exe import ExeNotFoundError, exec_exe


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    try:
        return exec_exe(args)
    except ExeNotFoundError as exc:
        print(f"hcsinfer: {exc}", file=sys.stderr)
        return 127


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
