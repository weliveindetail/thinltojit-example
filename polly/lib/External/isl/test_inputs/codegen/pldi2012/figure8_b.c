{
  for (int c0 = 2; c0 < n - 1; c0 += 4) {
    s1(c0);
    s0(c0 + 2);
  }
  if (n >= 1 && n % 4 >= 2)
    s1(-(n % 4) + n + 2);
}
