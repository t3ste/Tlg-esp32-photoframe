import "vuetify/styles";
import { createVuetify } from "vuetify";

export default createVuetify({
  theme: {
    defaultTheme: "light",
    themes: {
      light: {
        colors: {
          primary: "#6e66ed",
          secondary: "#424242",
          accent: "#82B1FF",
          error: "#FF5252",
          info: "#2196F3",
          success: "#4CAF50",
          warning: "#FB8C00",
        },
      },
    },
  },
  defaults: {
    VBtn: {
      variant: "flat",
    },
    VCard: {
      elevation: 2,
    },
  },
});
