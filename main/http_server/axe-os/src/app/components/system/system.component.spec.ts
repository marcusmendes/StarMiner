import { ComponentFixture, TestBed } from '@angular/core/testing';

import { SystemComponent } from './system.component';
import { provideHttpClient } from '@angular/common/http';
import { provideToastr } from 'ngx-toastr';

describe('SystemComponent', () => {
  let component: SystemComponent;
  let fixture: ComponentFixture<SystemComponent>;

  beforeEach(async () => {
    await TestBed.configureTestingModule({
      declarations: [SystemComponent],
      providers: [provideHttpClient(), provideToastr()]
    })
    .compileComponents();

    fixture = TestBed.createComponent(SystemComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });

  const baseInfo: any = {
    boardVersion: '602',
    uptimeSeconds: 120,
    resetReason: 'Power-on reset',
    dvfsProfile: 3,
    watchdogResets: 2,
    cmlErrorCount: 4,
    ssid: 'net',
    wifiStatus: 'Connected!',
    wifiRSSI: -45,
    ipv4: '192.168.1.2',
    ipv6: 'fe80::1',
    macAddr: '00:11:22:33:44:55',
    cpuUsage: 10,
    freeHeap: 1000,
    freeHeapInternal: 1000,
    freeHeapSpiram: 1000,
    minFreeHeap: 900,
    maxAllocHeap: 800,
    version: 'v2.12.0',
    axeOSVersion: 'v2.12.0',
    idfVersion: 'v5.5.1',
  };

  const baseAsic: any = {
    deviceModel: 'Gamma',
    swarmColor: 'purple',
    asicCount: 1,
    ASICModel: 'BM1370',
  };

  it('should include DVFS Profile (Performance), ASIC Watchdog Resets, and PMBus CML Recoveries rows', () => {
    const rows = component.getSystemRows({
      info: { ...baseInfo, dvfsProfile: 3, watchdogResets: 2, cmlErrorCount: 4 },
      asic: baseAsic,
    });

    expect(rows.find(r => r.label === 'DVFS Profile')?.value).toBe('Performance');
    expect(rows.find(r => r.label === 'Pre-Watchdog Recoveries')?.value).toBe('0');
    expect(rows.find(r => r.label === 'ASIC Watchdog Resets')?.value).toBe('2');
    expect(rows.find(r => r.label === 'PMBus CML Recoveries')?.value).toBe('4');
  });

  it('should format Adaptive DVFS Profile, Adaptive Safe Point, and Pre-Watchdog Recoveries rows', () => {
    const adaptiveRows = component.getSystemRows({
      info: {
        ...baseInfo,
        dvfsProfile: 4,
        adaptiveBestFreq: 587.5,
        adaptiveBestVcore: 1140,
        preWatchdogRecoveries: 2,
      },
      asic: baseAsic,
    });

    expect(adaptiveRows.find(r => r.label === 'DVFS Profile')?.value).toBe('Adaptive');
    expect(adaptiveRows.find(r => r.label === 'Adaptive Safe Point')?.value).toBe('587.5 MHz @ 1140 mV');
    expect(adaptiveRows.find(r => r.label === 'Pre-Watchdog Recoveries')?.value).toBe('2');
  });

  it('should format Balanced and Manual DVFS Profile rows and default missing counters to 0', () => {
    const balancedRows = component.getSystemRows({
      info: { ...baseInfo, dvfsProfile: 2, watchdogResets: 1, cmlErrorCount: 3 },
      asic: baseAsic,
    });
    expect(balancedRows.find(r => r.label === 'DVFS Profile')?.value).toBe('Balanced');
    expect(balancedRows.find(r => r.label === 'ASIC Watchdog Resets')?.value).toBe('1');
    expect(balancedRows.find(r => r.label === 'PMBus CML Recoveries')?.value).toBe('3');

    const manualRows = component.getSystemRows({
      info: { ...baseInfo, dvfsProfile: 0, watchdogResets: undefined, cmlErrorCount: undefined },
      asic: baseAsic,
    });
    expect(manualRows.find(r => r.label === 'DVFS Profile')?.value).toBe('Manual');
    expect(manualRows.find(r => r.label === 'ASIC Watchdog Resets')?.value).toBe('0');
    expect(manualRows.find(r => r.label === 'PMBus CML Recoveries')?.value).toBe('0');
  });
});
