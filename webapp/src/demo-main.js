import { createApp } from "vue";
import { createPinia } from "pinia";
import vuetify from "./plugins/vuetify";
import DemoPage from "./views/DemoPage.vue";

const app = createApp(DemoPage);

app.use(createPinia());
app.use(vuetify);

app.mount("#app");
