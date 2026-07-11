import { useState } from "react";
import { useNavigate, Link } from "react-router-dom";
import { useAuth } from "@/contexts/AuthContext";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import { useToast } from "@/hooks/use-toast";
import { Train, Shield } from "lucide-react";

const Login = () => {
  const [username, setUsername] = useState("");
  const [password, setPassword] = useState("");
  const [loading, setLoading] = useState(false);
  const { signIn } = useAuth();
  const navigate = useNavigate();
  const { toast } = useToast();

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    setLoading(true);
    const { error } = await signIn(username, password);
    setLoading(false);
    if (error) toast({ title: "Login Failed", description: error.message, variant: "destructive" });
    else navigate("/");
  };

  return (
    <div className="min-h-screen flex items-center justify-center p-4 railway-gradient">
      <Card className="w-full max-w-md bg-card border-border">
        <CardHeader className="text-center space-y-3">
          <div className="mx-auto w-14 h-14 rounded-xl railway-gradient flex items-center justify-center">
            <Train className="h-7 w-7 text-primary-foreground" />
          </div>
          <CardTitle className="text-xl text-foreground">Railway NVR System</CardTitle>
          <p className="text-sm text-muted-foreground">AI-Powered Mobile Network Video Recorder</p>
        </CardHeader>
        <CardContent>
          <form onSubmit={handleSubmit} className="space-y-4">
            <div><Label>Username</Label><Input placeholder="admin" value={username} onChange={(e) => setUsername(e.target.value)} required autoComplete="username" /></div>
            <div><Label>Password</Label><Input type="password" placeholder="••••••••" value={password} onChange={(e) => setPassword(e.target.value)} required autoComplete="current-password" /></div>
            <Button type="submit" className="w-full" disabled={loading}>{loading ? "Signing in..." : "Sign In"}</Button>
            <div className="text-center">
              <Link to="/register" className="text-xs text-primary hover:underline">Create an account</Link>
            </div>
            <div className="flex items-center gap-2 p-2 rounded-md bg-ai-surface/50 border border-ai-glow/20">
              <Shield className="h-4 w-4 text-ai-glow shrink-0" />
              <p className="text-[10px] text-muted-foreground">Default: admin / Admin@123 — change on first login</p>
            </div>
          </form>
        </CardContent>
      </Card>
    </div>
  );
};
export default Login;
