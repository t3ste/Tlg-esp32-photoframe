import { createRouter, createWebHistory } from "vue-router";
import MainApp from "../views/MainApp.vue";
import ProvisionPage from "../views/ProvisionPage.vue";
import DemoPage from "../views/DemoPage.vue";

const routes = [
  {
    path: "/",
    name: "main",
    component: MainApp,
  },
  {
    path: "/provision",
    name: "provision",
    component: ProvisionPage,
  },
  {
    path: "/demo",
    name: "demo",
    component: DemoPage,
  },
];

const router = createRouter({
  history: createWebHistory(),
  routes,
});

export default router;
