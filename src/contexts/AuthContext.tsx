import { createContext, useContext, useEffect, useState, ReactNode } from "react";
import { apiPost, apiGet, tokenStore } from "@/lib/api";

export interface NvrUser {
  user_id: number;
  username: string;
  full_name: string;
  email: string | null;
  role: "ADMIN" | "OPERATOR" | "MAINTENANCE" | "VIEWER";
}

interface AuthContextType {
  user: NvrUser | null;
  loading: boolean;
  signIn: (username: string, password: string) => Promise<{ error: Error | null }>;
  signUp: (username: string, password: string, fullName: string, email?: string) => Promise<{ error: Error | null }>;
  signOut: () => void;
  isAdmin: boolean;
  isOperator: boolean;
}

const AuthContext = createContext<AuthContextType>({} as AuthContextType);
export const useAuth = () => useContext(AuthContext);

export const AuthProvider = ({ children }: { children: ReactNode }) => {
  const [user, setUser] = useState<NvrUser | null>(null);
  const [loading, setLoading] = useState(true);

  useEffect(() => {
    const token = tokenStore.get();
    if (!token) { setLoading(false); return; }
    apiGet<NvrUser>("/api/auth/me")
      .then(setUser)
      .catch(() => { tokenStore.clear(); })
      .finally(() => setLoading(false));
  }, []);

  const signIn = async (username: string, password: string) => {
    try {
      const data = await apiPost<{ token: string; user: NvrUser }>("/api/auth/login", { username, password });
      tokenStore.set(data.token);
      setUser(data.user);
      return { error: null };
    } catch (err) { return { error: err as Error }; }
  };

  const signUp = async (username: string, password: string, fullName: string, email?: string) => {
    try {
      const data = await apiPost<{ token: string; user: NvrUser }>("/api/auth/register", { username, password, full_name: fullName, email });
      tokenStore.set(data.token);
      setUser(data.user);
      return { error: null };
    } catch (err) { return { error: err as Error }; }
  };

  const signOut = () => { tokenStore.clear(); setUser(null); };

  return (
    <AuthContext.Provider value={{
      user, loading, signIn, signUp, signOut,
      isAdmin:    user?.role === "ADMIN",
      isOperator: user?.role === "ADMIN" || user?.role === "OPERATOR",
    }}>
      {children}
    </AuthContext.Provider>
  );
};
