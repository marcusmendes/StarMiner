import { ComponentFixture, TestBed } from '@angular/core/testing';
import { FormsModule, ReactiveFormsModule } from '@angular/forms';
import { NoopAnimationsModule } from '@angular/platform-browser/animations';
import { provideHttpClient } from '@angular/common/http';
import { provideToastr } from 'ngx-toastr';
import { provideRouter } from '@angular/router';
import { BehaviorSubject, of } from 'rxjs';
import { MessageModule } from 'primeng/message';
import { TooltipModule } from 'primeng/tooltip';

import { EditComponent } from './edit.component';
import { PrimeNGModule } from 'src/app/prime-ng.module';
import { DateAgoPipe } from 'src/app/pipes/date-ago.pipe';
import { LiveDataService } from 'src/app/services/live-data.service';
import { SystemApiService } from 'src/app/services/system.service';

describe('EditComponent', () => {
  let component: EditComponent;
  let fixture: ComponentFixture<EditComponent>;
  let infoSubject: BehaviorSubject<any>;
  let systemApiSpy: jasmine.SpyObj<SystemApiService>;

  const baseInfo = {
    display: 'SSD1306 (128x32)',
    rotation: 0,
    invertscreen: 0,
    displayTimeout: -1,
    coreVoltage: 1165,
    targetVoltage: 1150,
    frequency: 475,
    targetFrequency: 550,
    dvfsProfile: 2,
    autofanspeed: 1,
    minFanSpeed: 25,
    manualFanSpeed: 70,
    temptarget: 60,
    overheat_mode: 0,
    statsFrequency: 30,
    statsLimit: 720,
    overclockEnabled: 0,
  };

  beforeEach(() => {
    infoSubject = new BehaviorSubject<any>(baseInfo);
    systemApiSpy = jasmine.createSpyObj('SystemApiService', ['getAsicSettings', 'updateSystem', 'getInfo', 'restart']);
    systemApiSpy.getAsicSettings.and.returnValue(of({
      ASICModel: 'BM1370' as any,
      deviceModel: 'Gamma',
      swarmColor: 'purple',
      asicCount: 1,
      defaultFrequency: 485,
      frequencyOptions: [400, 485, 550],
      defaultVoltage: 1200,
      voltageOptions: [1100, 1150, 1200],
    }));
    systemApiSpy.updateSystem.and.returnValue(of(undefined));

    TestBed.configureTestingModule({
      declarations: [EditComponent, DateAgoPipe],
      imports: [
        ReactiveFormsModule,
        FormsModule,
        NoopAnimationsModule,
        PrimeNGModule,
        MessageModule,
        TooltipModule,
      ],
      providers: [
        provideHttpClient(),
        provideToastr(),
        provideRouter([]),
        { provide: LiveDataService, useValue: { info$: infoSubject.asObservable() } },
        { provide: SystemApiService, useValue: systemApiSpy },
      ]
    });
    fixture = TestBed.createComponent(EditComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });

  it('should initialize frequency and coreVoltage from targetFrequency and targetVoltage when present', () => {
    expect(component.form.get('frequency')?.value).toBe(550);
    expect(component.form.get('coreVoltage')?.value).toBe(1150);
    expect(component.form.get('dvfsProfile')?.value).toBe(2);
  });

  it('should save dvfsProfile changes via updateSystem without requiring restart', () => {
    const dvfsControl = component.form.get('dvfsProfile')!;
    dvfsControl.setValue(3);
    dvfsControl.markAsDirty();

    expect(component.isRestartRequired).toBeFalse();

    component.updateSystem();

    expect(systemApiSpy.updateSystem).toHaveBeenCalledWith(
      '',
      jasmine.objectContaining({
        frequency: 550,
        coreVoltage: 1150,
        dvfsProfile: 3,
      })
    );
  });
});
