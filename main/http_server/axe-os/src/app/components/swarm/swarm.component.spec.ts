import { ComponentFixture, TestBed } from '@angular/core/testing';
import { provideHttpClient } from '@angular/common/http';
import { provideHttpClientTesting } from '@angular/common/http/testing';
import { FormsModule, ReactiveFormsModule } from '@angular/forms';
import { NoopAnimationsModule } from '@angular/platform-browser/animations';
import { provideToastr } from 'ngx-toastr';
import { ButtonModule } from 'primeng/button';
import { DropdownModule } from 'primeng/dropdown';
import { FileUploadModule } from 'primeng/fileupload';
import { InputGroupModule } from 'primeng/inputgroup';
import { InputTextModule } from 'primeng/inputtext';
import { SliderModule } from 'primeng/slider';
import { TooltipModule } from 'primeng/tooltip';

import { DateAgoPipe } from 'src/app/pipes/date-ago.pipe';
import { DiffSuffixPipe } from 'src/app/pipes/diff-suffix.pipe';
import { HashSuffixPipe } from 'src/app/pipes/hash-suffix.pipe';
import { ModalComponent } from '../modal/modal.component';
import { SwarmComponent } from './swarm.component';

describe('SwarmComponent', () => {
  let component: SwarmComponent;
  let fixture: ComponentFixture<SwarmComponent>;

  beforeEach(() => {
    localStorage.clear();
    TestBed.configureTestingModule({
      declarations: [
        SwarmComponent,
        ModalComponent,
        HashSuffixPipe,
        DiffSuffixPipe,
        DateAgoPipe
      ],
      imports: [
        FormsModule,
        ReactiveFormsModule,
        NoopAnimationsModule,
        ButtonModule,
        DropdownModule,
        SliderModule,
        TooltipModule,
        InputTextModule,
        FileUploadModule,
        InputGroupModule
      ],
      providers: [provideHttpClient(), provideHttpClientTesting(), provideToastr()]
    });
    fixture = TestBed.createComponent(SwarmComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });

  describe('fleet J/TH efficiency calculation', () => {
    it('computes per-device efficiency and fleet totals.efficiency', () => {
      const dev1: any = {
        address: 'gamma-1.local',
        ipv4: '192.168.1.10',
        ASICModel: 'BM1370',
        deviceModel: 'Gamma',
        swarmColor: 'green',
        asicCount: 1,
        hashRate: 1200, // 1.2 TH/s
        power: 18.0,    // 15.0 J/TH
        bestDiff: 1000,
        sharesAccepted: 10,
        sharesRejected: 0,
        uptimeSeconds: 100,
        temp: 60,
        poolDifficulty: 1000,
        version: 'v2.12.0',
        dvfsProfile: 1,
        frequency: 550,
        actualFrequency: 550,
        coreVoltage: 1150,
        vdroopOffsetMv: 10,
        learnedMaxFreq: 612.5,
        learnedMinVcore: 1140,
      };
      const dev2: any = {
        address: 'gamma-2.local',
        ipv4: '192.168.1.11',
        ASICModel: 'BM1370',
        deviceModel: 'Gamma',
        swarmColor: 'green',
        asicCount: 1,
        hashRate: 800,  // 0.8 TH/s
        power: 16.0,    // 20.0 J/TH
        bestDiff: 2000,
        sharesAccepted: 20,
        sharesRejected: 1,
        uptimeSeconds: 200,
        temp: 62,
        poolDifficulty: 1000,
        version: 'v2.12.0',
        dvfsProfile: 2,
        frequency: 575,
        actualFrequency: 575,
        coreVoltage: 1180,
      };

      expect(component.getDeviceEfficiency(dev1)).toBeCloseTo(15.0, 2);
      expect(component.getDeviceEfficiency(dev2)).toBeCloseTo(20.0, 2);
      expect(component.getDeviceEfficiency({ hashRate: 0, power: 15 })).toBe(0);
      expect(component.getDeviceEfficiency({ hashRate: 1000, power: 15, power_fault: 'Fault' })).toBe(0);

      component.swarm = [dev1, dev2];
      component['calculateTotals']();

      expect(component.totals.hashRate).toBe(2000);
      expect(component.totals.power).toBe(34.0);
      // Fleet J/TH = 34.0 W / (2000 GH/s / 1000) = 17.0 J/TH
      expect(component.totals.efficiency).toBeCloseTo(17.0, 2);
      expect(dev1['efficiency']).toBeCloseTo(15.0, 2);
      expect(dev2['efficiency']).toBeCloseTo(20.0, 2);

      fixture.detectChanges();
      const text = fixture.nativeElement.textContent as string;
      expect(text).toContain('Fleet Efficiency');
      expect(text).toContain('17.0 J/TH');
      expect(text).toContain('15.0 J/TH');
      expect(text).toContain('20.0 J/TH');
      expect(text).toContain('Efficiency');
      expect(text).toContain('Balanced');
      expect(text).toContain('(+10 mV)');
    });
  });

  describe('legacy device compatibility and DVFS helpers', () => {
    it('handles legacy devices missing DVFS, Vdroop, and watchdog fields in list and grid views', () => {
      const legacyDevice: any = {
        address: 'legacy.local',
        ipv4: '192.168.1.50',
        ASICModel: 'BM1366',
        deviceModel: 'Ultra',
        swarmColor: 'purple',
        asicCount: 1,
        hashRate: 500,
        power: 12.5,
        bestDiff: 500,
        sharesAccepted: 5,
        sharesRejected: 0,
        uptimeSeconds: 600,
        temp: 58,
        poolDifficulty: 500,
        version: 'v2.4.0',
        frequency: 485,
        coreVoltage: 1200,
      };

      expect(component.getDvfsProfileLabel(legacyDevice.dvfsProfile)).toBe('Manual');
      expect(component.getDvfsProfileLabel(1)).toBe('Efficiency');
      expect(component.getDvfsProfileLabel(2)).toBe('Balanced');
      expect(component.getDvfsProfileLabel(3)).toBe('Performance');
      expect(component.getLearnedBoundsTooltip(legacyDevice)).toBe('');
      expect(component.getLearnedBoundsTooltip({ learnedMaxFreq: 612.5, learnedMinVcore: 1145 }))
        .toBe('Learned silicon bounds (Max Freq: 612.5 MHz, Min VCore: 1145 mV)');

      component.swarm = [legacyDevice];
      component['calculateTotals']();

      component.gridView = false;
      fixture.detectChanges();
      let text = fixture.nativeElement.textContent as string;
      expect(text).toContain('Manual');
      expect(text).toContain('25.0 J/TH');
      expect(text).toContain('485 MHz');
      expect(text).toContain('1200 mV');
      expect(text).not.toContain('undefined');
      expect(text).not.toContain('NaN');

      component.gridView = true;
      fixture.detectChanges();
      text = fixture.nativeElement.textContent as string;
      expect(text).toContain('Manual');
      expect(text).toContain('25.0 J/TH');
      expect(text).not.toContain('undefined');
      expect(text).not.toContain('NaN');
    });
  });

  describe('getDeviceNotification()', () => {
    it('alerts on Thermal Throttling and Watchdog Resets without false Frequency Low', () => {
      expect(component.getDeviceNotification({
        frequency: 387.5,
        targetFrequency: 550,
        throttleActive: 1,
      })).toEqual({ color: 'orange', msg: 'Thermal Throttling' });

      expect(component.getDeviceNotification({
        frequency: 387.5,
        targetFrequency: 550,
        throttleActive: 0,
        watchdogResets: 2,
      })).toEqual({ color: 'orange', msg: 'Watchdog Resets' });

      expect(component.getDeviceNotification({
        frequency: 387.5,
        targetFrequency: 550,
        throttleActive: 0,
        watchdogResets: 0,
      })).toBeUndefined();
    });

    it('uses targetFrequency ?? frequency for Frequency Low and preserves critical alert priority', () => {
      expect(component.getDeviceNotification({
        frequency: 350,
        targetFrequency: 350,
        throttleActive: 0,
      })).toEqual({ color: 'orange', msg: 'Frequency Low' });

      expect(component.getDeviceNotification({
        frequency: 350,
      })).toEqual({ color: 'orange', msg: 'Frequency Low' });

      expect(component.getDeviceNotification({
        miningPaused: true,
        throttleActive: 1,
        watchdogResets: 3,
      })).toEqual({ color: 'yellow', msg: 'Paused' });

      expect(component.getDeviceNotification({
        overheat_mode: 1,
        throttleActive: 1,
        watchdogResets: 3,
      })).toEqual({ color: 'red', msg: 'Overheated' });

      expect(component.getDeviceNotification({
        power_fault: 'VOUT_OV',
        throttleActive: 1,
        watchdogResets: 3,
      })).toEqual({ color: 'red', msg: 'Power Fault' });
    });
  });
});
