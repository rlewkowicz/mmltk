import { Routes } from '@angular/router';
import { WorkflowIndexRedirectComponent } from './workflow-index-redirect.component';
import { WorkflowShellComponent } from './workflow-shell.component';

export const routes: Routes = [
  {
    path: '',
    pathMatch: 'full',
    component: WorkflowIndexRedirectComponent,
  },
  {
    path: 'train',
    component: WorkflowShellComponent,
    data: { workflow: 'train' },
  },
  {
    path: 'validate',
    component: WorkflowShellComponent,
    data: { workflow: 'validate' },
  },
  {
    path: 'predict',
    component: WorkflowShellComponent,
    data: { workflow: 'predict' },
  },
  {
    path: 'annotate',
    component: WorkflowShellComponent,
    data: { workflow: 'annotate' },
  },
  {
    path: 'export',
    component: WorkflowShellComponent,
    data: { workflow: 'export' },
  },
  {
    path: 'live',
    component: WorkflowShellComponent,
    data: { workflow: 'live' },
  },
  {
    path: '**',
    redirectTo: '',
  },
];
