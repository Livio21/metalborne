"""Host TLS segment used by prepared x86-64 game images."""
import os
import sys


def guest_tls_segment():
    segment = os.environ.get('BB_GUEST_TLS', 'fs' if sys.platform == 'darwin' else 'gs')
    if segment not in ('fs', 'gs'):
        raise ValueError('BB_GUEST_TLS must be fs (macOS) or gs (Linux)')
    return segment
