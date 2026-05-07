import { ChangeDetectionStrategy, Component } from '@angular/core';

@Component({
  selector: 'app-topbar',
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    <header class="topbar">
      <div class="brand-block">
        <h1>mmltk</h1>
      </div>
    </header>
  `,
})
export class TopbarComponent {}
