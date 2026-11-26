import { Pipe, PipeTransform } from '@angular/core';

@Pipe({
  name: 'sats',
  pure: true
})
export class SatsPipe implements PipeTransform {
  private static _this = new SatsPipe();

  public static transform(value: number, ticker: string = 'BTC', decimals: number = 8): string {
    return this._this.transform(value, ticker, decimals);
  }

  transform(value: number, ticker: string = 'BTC', decimals: number = 8): string {
    const safeTicker = ticker || 'BTC';
    const safeDecimals = (decimals !== undefined && decimals !== null && decimals >= 0 && decimals <= 12) ? decimals : 8;
    if (!value) return `0 ${safeTicker}`;
    const divisor = Math.pow(10, safeDecimals);
    return `${(value / divisor).toFixed(safeDecimals)} ${safeTicker}`;
  }
}
