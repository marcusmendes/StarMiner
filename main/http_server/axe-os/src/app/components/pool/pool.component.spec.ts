import { ComponentFixture, TestBed } from '@angular/core/testing';
import { NO_ERRORS_SCHEMA } from '@angular/core';
import { provideHttpClient } from '@angular/common/http';
import { ReactiveFormsModule } from '@angular/forms';
import { NoopAnimationsModule } from '@angular/platform-browser/animations';
import { provideToastr } from 'ngx-toastr';
import { DropdownModule } from 'primeng/dropdown';
import { CheckboxModule } from 'primeng/checkbox';
import { RadioButtonModule } from 'primeng/radiobutton';
import { of } from 'rxjs';

import { PoolComponent } from './pool.component';
import { SystemApiService } from 'src/app/services/system.service';
import { LiveDataService } from 'src/app/services/live-data.service';
import { LoadingService } from 'src/app/services/loading.service';
import { TooltipTextIconComponent } from 'src/app/components/tooltip-text-icon/tooltip-text-icon.component';

describe('PoolComponent', () => {
  let component: PoolComponent;
  let fixture: ComponentFixture<PoolComponent>;
  let systemServiceSpy: jasmine.SpyObj<SystemApiService>;

  beforeEach(() => {
    systemServiceSpy = jasmine.createSpyObj('SystemApiService', ['updateSystem', 'restart']);
    systemServiceSpy.updateSystem.and.returnValue(of({} as any));

    const mockInfo: any = {
      ASICModel: 'BM1370',
      stratumURL: 'lat-dgb.solofury.com',
      stratumPort: 3333,
      stratumProtocol: 'SV1',
      stratumUser: 'DJKYme7vLUcgDAJGqjXbcmqdRVWnH5xQnU.Bitaxe01',
      stratumSuggestedDifficulty: 1000,
      stratumExtranonceSubscribe: false,
      stratumTLS: 0,
      stratumCert: '',
      stratumDecodeCoinbase: true,
      stratumCoinProfile: 2,
      stratumV2AuthorityPubkey: '',
      stratumV2ChannelType: 'extended',
      fallbackStratumURL: 'solo.ckpool.org',
      fallbackStratumPort: 3333,
      fallbackStratumProtocol: 'SV1',
      fallbackStratumUser: 'bc1qtest.bitaxe',
      fallbackStratumSuggestedDifficulty: 1000,
      fallbackStratumExtranonceSubscribe: false,
      fallbackStratumTLS: 0,
      fallbackStratumCert: '',
      fallbackStratumDecodeCoinbase: true,
      fallbackStratumCoinProfile: 1,
      fallbackStratumV2AuthorityPubkey: '',
      fallbackStratumV2ChannelType: 'extended'
    };

    const mockLiveDataService = {
      info$: of(mockInfo)
    };

    const mockLoadingService = {
      lockUIUntilComplete: () => (source$: any) => source$
    };

    TestBed.configureTestingModule({
      declarations: [PoolComponent, TooltipTextIconComponent],
      imports: [ReactiveFormsModule, NoopAnimationsModule, DropdownModule, CheckboxModule, RadioButtonModule],
      providers: [
        provideHttpClient(),
        provideToastr(),
        { provide: SystemApiService, useValue: systemServiceSpy },
        { provide: LiveDataService, useValue: mockLiveDataService },
        { provide: LoadingService, useValue: mockLoadingService }
      ],
      schemas: [NO_ERRORS_SCHEMA]
    });

    fixture = TestBed.createComponent(PoolComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create and initialize primary and fallback coin profile controls', () => {
    expect(component).toBeTruthy();
    expect(component.form.get('stratumCoinProfile')?.value).toBe(2);
    expect(component.form.get('fallbackStratumCoinProfile')?.value).toBe(1);
    expect(component.coinProfileOptions.length).toBe(11);
  });

  it('should reject out-of-range coin profile values and submit valid coin profiles', () => {
    const primaryCtrl = component.form.get('stratumCoinProfile');
    primaryCtrl?.setValue(11);
    expect(primaryCtrl?.invalid).toBeTrue();

    primaryCtrl?.setValue(7);
    expect(primaryCtrl?.valid).toBeTrue();

    component.updateSystem();
    expect(systemServiceSpy.updateSystem).toHaveBeenCalled();
    const payload = systemServiceSpy.updateSystem.calls.mostRecent().args[1];
    expect(payload.stratumCoinProfile).toBe(7);
    expect(payload.fallbackStratumCoinProfile).toBe(1);
  });
});
