"""Matched statistics for the causal analysis behind the "why" view (standard library only).

Each down is a stratum: the downed player (the case) against everyone on the same side under fire at that moment.
- mh_or: Mantel-Haenszel odds ratio of a binary exposure across matched sets, with a Robins-Breslow-Greenland 95% CI.
- clr: conditional logistic regression (Newton), coefficients and standard errors.
"""
import csv
import math
import os

# the extractor's CSV tables: $CAUSAL_DATA, else data/causal in the project (gitignored)
OUT = os.environ.get("CAUSAL_DATA") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "data", "causal")


def load(name, num=True):
    with open(os.path.join(OUT, name), newline="") as f:
        r = csv.DictReader(f)
        rows = []
        for row in r:
            if num:
                for k, v in row.items():
                    try:
                        row[k] = float(v) if ("." in v or "e" in v) else int(v)
                    except ValueError:
                        pass
            rows.append(row)
        return rows


def strata(rows, key="ev"):
    out = {}
    for row in rows:
        out.setdefault(row[key], []).append(row)
    return out


def mh_or(groups, expo):
    """groups: list of lists of (case, member dict); expo(member) -> 0/1 (or None to drop the member).
    Returns (OR, lo, hi, exposed cases, cases, exposed members share)."""
    R = S = 0.0
    PR = PS_QR = QS = 0.0
    sumR = sumS = 0.0
    a_tot = n_cases = 0
    exp_members = members = 0
    for g in groups:
        a = b = c = d = 0
        for case, m in g:
            e = expo(m)
            if e is None:
                continue
            members += 1
            exp_members += e
            if case:
                a += e
                c += 1 - e
            else:
                b += e
                d += 1 - e
        n = a + b + c + d
        if n < 2 or a + c == 0:
            continue
        n_cases += a + c
        a_tot += a
        r = a * d / n
        s = b * c / n
        p = (a + d) / n
        q = (b + c) / n
        R += r
        S += s
        PR += p * r
        PS_QR += p * s + q * r
        QS += q * s
    if R == 0 or S == 0:
        return None
    OR = R / S
    var = PR / (2 * R * R) + PS_QR / (2 * R * S) + QS / (2 * S * S)
    se = math.sqrt(var)
    return OR, math.exp(math.log(OR) - 1.96 * se), math.exp(math.log(OR) + 1.96 * se), a_tot, n_cases, exp_members / max(1, members)


def solve(A, b):
    n = len(b)
    M = [row[:] + [b[i]] for i, row in enumerate(A)]
    for i in range(n):
        piv = max(range(i, n), key=lambda r: abs(M[r][i]))
        M[i], M[piv] = M[piv], M[i]
        if abs(M[i][i]) < 1e-12:
            M[i][i] = 1e-12
        for r in range(n):
            if r != i:
                f = M[r][i] / M[i][i]
                if f:
                    for k in range(i, n + 1):
                        M[r][k] -= f * M[i][k]
    return [M[i][n] / M[i][i] for i in range(n)]


def invert(A):
    n = len(A)
    cols = []
    for j in range(n):
        e = [0.0] * n
        e[j] = 1.0
        cols.append(solve(A, e))
    return [[cols[j][i] for j in range(n)] for i in range(n)]


def clr(groups, names, iters=8, ridge=1e-4):
    """groups: list of (case_index, [x vectors]). Returns beta, se, loglik."""
    k = len(names)
    beta = [0.0] * k
    ll = 0.0
    for it in range(iters):
        g = [0.0] * k
        H = [[0.0] * k for _ in range(k)]
        ll = 0.0
        for ci, xs in groups:
            etas = [sum(b * x for b, x in zip(beta, xv)) for xv in xs]
            mx = max(etas)
            ws = [math.exp(e - mx) for e in etas]
            sw = sum(ws)
            ll += etas[ci] - mx - math.log(sw)
            mean = [0.0] * k
            for w, xv in zip(ws, xs):
                w /= sw
                for a in range(k):
                    mean[a] += w * xv[a]
            for a in range(k):
                g[a] += xs[ci][a] - mean[a]
            for w, xv in zip(ws, xs):
                w /= sw
                for a in range(k):
                    da = xv[a] - mean[a]
                    if da == 0:
                        continue
                    row = H[a]
                    for c in range(a, k):
                        row[c] += w * da * (xv[c] - mean[c])
        for a in range(k):
            for c in range(a):
                H[a][c] = H[c][a]
            H[a][a] += ridge
        step = solve(H, g)
        beta = [b + s for b, s in zip(beta, step)]
        if max(abs(s) for s in step) < 1e-6:
            break
    cov = invert(H)
    se = [math.sqrt(max(cov[i][i], 0)) for i in range(k)]
    return beta, se, ll


def fmt_or(res):
    if not res:
        return "n/a"
    OR, lo, hi, a, n, share = res
    return f"OR {OR:5.2f} [{lo:4.2f}-{hi:4.2f}]  exposed cases {a}/{n} ({100*a/max(1,n):4.1f}%)  exposed members {100*share:4.1f}%"


def logit(rows, names, iters=10, ridge=1e-4):
    """rows: list of (y, x vector) with x[0] = 1 for the intercept. Returns beta, se."""
    k = len(names)
    beta = [0.0] * k
    for it in range(iters):
        g = [0.0] * k
        H = [[0.0] * k for _ in range(k)]
        for y, x in rows:
            eta = sum(b * v for b, v in zip(beta, x))
            p = 1 / (1 + math.exp(-max(-30, min(30, eta))))
            w = p * (1 - p)
            for a in range(k):
                if x[a] == 0:
                    continue
                g[a] += (y - p) * x[a]
                row = H[a]
                for c in range(a, k):
                    row[c] += w * x[a] * x[c]
        for a in range(k):
            for c in range(a):
                H[a][c] = H[c][a]
            H[a][a] += ridge
        step = solve(H, g)
        beta = [b + s for b, s in zip(beta, step)]
        if max(abs(s) for s in step) < 1e-7:
            break
    cov = invert(H)
    return beta, [math.sqrt(max(cov[i][i], 0)) for i in range(k)]
