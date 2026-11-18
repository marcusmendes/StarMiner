import { ComponentFixture, TestBed } from '@angular/core/testing';
import { HomeComponent } from './home.component';
import { provideHttpClient } from '@angular/common/http';
import { provideToastr } from 'ngx-toastr';
import { ReactiveFormsModule, FormsModule } from '@angular/forms';
import { NoopAnimationsModule } from '@angular/platform-browser/animations';
import { Title } from '@angular/platform-browser';
import { provideRouter } from '@angular/router';
import { MessageModule } from 'primeng/message';
import { DropdownModule } from 'primeng/dropdown';
import { ChartModule } from 'primeng/chart';
import { ProgressBarModule } from 'primeng/progressbar';
import { TooltipModule } from 'primeng/tooltip';

import { HashSuffixPipe } from 'src/app/pipes/hash-suffix.pipe';
import { DiffSuffixPipe } from 'src/app/pipes/diff-suffix.pipe';
import { DateAgoPipe } from 'src/app/pipes/date-ago.pipe';
import { AddressPipe } from 'src/app/pipes/address.pipe';
import { SatsPipe } from 'src/app/pipes/sats.pipe';
import { ByteSuffixPipe } from 'src/app/pipes/byte-suffix.pipe';

import { TooltipTextIconComponent } from 'src/app/components/tooltip-text-icon/tooltip-text-icon.component';
import { ConfettiComponent } from 'src/app/components/confetti/confetti.component';
import { SnowflakesComponent } from 'src/app/components/snowflakes/snowflakes.component';

import { SystemApiService } from 'src/app/services/system.service';
import { ThemeService } from 'src/app/services/theme.service';
import { QuicklinkService } from 'src/app/services/quicklink.service';
import { LoadingService } from 'src/app/services/loading.service';
import { ShareRejectionExplanationService } from 'src/app/services/share-rejection-explanation.service';
import { LocalStorageService } from 'src/app/local-storage.service';
import { DashboardEditService } from 'src/app/services/dashboard-edit.service';
import { LayoutService } from 'src/app/layout/service/app.layout.service';

describe('HomeComponent', () => {
  let component: HomeComponent;
  let fixture: ComponentFixture<HomeComponent>;

  beforeEach(() => {
    TestBed.configureTestingModule({
      declarations: [
        HomeComponent,
        TooltipTextIconComponent,
        ConfettiComponent,
        SnowflakesComponent,
        HashSuffixPipe,
        DiffSuffixPipe,
        DateAgoPipe,
        AddressPipe,
        SatsPipe,
        ByteSuffixPipe
      ],
      imports: [
        ReactiveFormsModule,
        FormsModule,
        NoopAnimationsModule,
        MessageModule,
        DropdownModule,
        ChartModule,
        ProgressBarModule,
        TooltipModule
      ],
      providers: [
        provideRouter([]),
        provideHttpClient(),
        provideToastr(),
        SystemApiService,
        ThemeService,
        QuicklinkService,
        Title,
        LoadingService,
        ShareRejectionExplanationService,
        LocalStorageService,
        DashboardEditService,
        LayoutService
      ]
    });
    fixture = TestBed.createComponent(HomeComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });

  describe('stale data and visibility state', () => {
    it('should set stale data error when visible and last message is old', () => {
      spyOnProperty(document, 'visibilityState', 'get').and.returnValue('visible');

      component['lastMessageTime'] = Date.now() - 10000;
      component.systemInfoError$.next({ duration: 0, startTime: null });

      component['checkStaleData']();

      expect(component.systemInfoError$.value.duration).toBe(10);
    });

    it('should NOT set stale data error when hidden and last message is old', () => {
      spyOnProperty(document, 'visibilityState', 'get').and.returnValue('hidden');

      component['lastMessageTime'] = Date.now() - 10000;
      component.systemInfoError$.next({ duration: 0, startTime: null });

      component['checkStaleData']();

      expect(component.systemInfoError$.value.duration).toBe(0);
    });

    it('should reset lastMessageTime and clear stale error when transitioning to visible', () => {
      spyOnProperty(document, 'visibilityState', 'get').and.returnValue('visible');

      const initialTime = Date.now() - 10000;
      component['lastMessageTime'] = initialTime;
      component.systemInfoError$.next({ duration: 10, startTime: initialTime });

      component.onVisibilityChange();

      expect(component.systemInfoError$.value.duration).toBe(0);
      expect(component.systemInfoError$.value.startTime).toBeNull();
      expect(component['lastMessageTime']).toBeGreaterThan(initialTime);
    });
  });

  describe('thermal throttling and target frequency messages', () => {
    const baseSystemInfo: any = {
      frequency: 500,
      actualFrequency: 500,
      targetFrequency: 550,
      coreVoltage: 1.15,
      targetVoltage: 1.15,
      throttleActive: 0,
      overheat_mode: 0,
      version: 'v2.12.0',
      axeOSVersion: 'v2.12.0',
      isUsingFallbackStratum: 0,
      coinbaseOutputs: [],
    };

    it('should add THERMAL_THROTTLE warn message without FREQUENCY_LOW when throttleActive is 1 and remove it when 0', () => {
      component.handleSystemMessages(
        { ...baseSystemInfo, frequency: 387.5, actualFrequency: 387.5, targetFrequency: 550, throttleActive: 1 },
        { duration: 0, startTime: null }
      );

      const throttleMsg = component.messages.find(m => m.type === 'THERMAL_THROTTLE');
      expect(throttleMsg).toBeDefined();
      expect(throttleMsg?.severity).toBe('warn');
      expect(throttleMsg?.text).toContain('388 / 550 MHz');
      expect(component.messages.find(m => m.type === 'FREQUENCY_LOW')).toBeUndefined();

      component.handleSystemMessages(
        { ...baseSystemInfo, frequency: 550, actualFrequency: 550, targetFrequency: 550, throttleActive: 0 },
        { duration: 0, startTime: null }
      );

      expect(component.messages.find(m => m.type === 'THERMAL_THROTTLE')).toBeUndefined();
      expect(component.messages.find(m => m.type === 'FREQUENCY_LOW')).toBeUndefined();
    });

    it('should emit FREQUENCY_LOW only when targetFrequency < 400', () => {
      component.handleSystemMessages(
        { ...baseSystemInfo, frequency: 387.5, actualFrequency: 387.5, targetFrequency: 400, throttleActive: 1 },
        { duration: 0, startTime: null }
      );

      expect(component.messages.find(m => m.type === 'FREQUENCY_LOW')).toBeUndefined();

      component.handleSystemMessages(
        { ...baseSystemInfo, frequency: 350, actualFrequency: 350, targetFrequency: 350, throttleActive: 0 },
        { duration: 0, startTime: null }
      );

      expect(component.messages.find(m => m.type === 'FREQUENCY_LOW')).toBeDefined();
    });

    it('should return expected labels from getDvfsProfileLabel', () => {
      expect(component.getDvfsProfileLabel(0)).toBe('Manual');
      expect(component.getDvfsProfileLabel(1)).toBe('Efficiency');
      expect(component.getDvfsProfileLabel(2)).toBe('Balanced');
      expect(component.getDvfsProfileLabel(3)).toBe('Performance');
    });
  });
});
