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

  it('should include DVFS Profile, ASIC Watchdog Resets, and PMBus CML Recoveries rows', () => {
    const rows = component.getSystemRows({
      info: {
        boardVersion: '602',
        uptimeSeconds: 120,
        resetReason: 'Power-on reset',
        dvfsProfile: 2,
        watchdogResets: 1,
        cmlErrorCount: 3,
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
      } as any,
      asic: {
        deviceModel: 'Gamma',
        swarmColor: 'purple',
        asicCount: 1,
        ASICModel: 'BM1370',
      } as any,
    });

    expect(rows.find(r => r.label === 'DVFS Profile')?.value).toBe('Balanced');
    expect(rows.find(r => r.label === 'ASIC Watchdog Resets')?.value).toBe('1');
    expect(rows.find(r => r.label === 'PMBus CML Recoveries')?.value).toBe('3');
  });
});
