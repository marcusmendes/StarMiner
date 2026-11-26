import { SatsPipe } from './sats.pipe';

describe('SatsPipe', () => {
  const pipe = new SatsPipe();

  it('create an instance', () => {
    expect(pipe).toBeTruthy();
  });

  it('formats zero and default BTC values cleanly', () => {
    expect(pipe.transform(0)).toBe('0 BTC');
    expect(pipe.transform(100_000_000)).toBe('1.00000000 BTC');
  });

  it('formats DGB with 8 decimals', () => {
    expect(pipe.transform(24_516_372_618, 'DGB', 8)).toBe('245.16372618 DGB');
    expect(pipe.transform(0, 'DGB', 8)).toBe('0 DGB');
  });

  it('formats XEC with 2 decimals', () => {
    expect(pipe.transform(123_456, 'XEC', 2)).toBe('1234.56 XEC');
  });
});
