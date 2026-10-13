#!/usr/bin/env python3
"""Command-line entry point for local stress campaign orchestration."""

import sys

from local_stress_campaign import main


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
